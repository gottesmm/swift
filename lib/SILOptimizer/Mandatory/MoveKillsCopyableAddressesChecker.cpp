//===--- MoveKillsCopyableAddressesChecker.cpp ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// In this file, we implement a checker that for memory objects in SIL checks
/// that after a call to _move, one can no longer use a var or a let with an
/// address only type. If you use it after that point, but before a known
/// reinitialization point, you get an error. Example:
///
///   var x = 5
///   let y = _move(x)
///   _use(x) // error!
///   x = 10
///   _use(x) // Ok, we reinitialized the memory.
///
/// Below, I describe in detail the algorithm.
///
/// # Design
///
/// ## Introduction
///
/// At its heart this checker is a dataflow checker that checks the lifetime of
/// a specific address. It uses AccessUseVisitor to reliably get all users of an
/// address. Then it partitions those uses into three sets: a set of moves, a
/// set of inits, and a set of misc uses that just require liveness. We call the
/// last category "livenessUses".
///
/// ## Marshalling the Uses
///
/// Then we marshal this information into per basic block info, performing
/// initial diagnostic checking. Specifically, we first visit moves, then inits,
/// and then liveness uses.
///
/// Visiting Moves. When we visit a move, we scan down the block and:
///
/// 1. If we visit any "liveness use", immediately emit an error diagnostic and
///    return.
/// 2. If we visit an init instruction, we stop scanning.
///
/// If we reach the end of the block, then we know that this block propagates to
/// its successors a move. We mark the block as being moveOut. Then we scan up
/// until we hit either an init or a use. If we do not hit either of those, then
/// we mark this block as propagating a use upwards. This is because a _move
/// could be an invalid use of a different _move.
///
/// Visiting Memory Initializations. If we have an initialization, we scan up
/// the block. If we see a use or a move, we stop. This block is not move out.
///
/// Visiting Liveness Uses. Then we search up from each liveness use. If we see
/// a liveness use, a _move, or a reinit, we stop processing. Otherwise, we mark
/// this block as propagating use liveness upwards.
///
/// ## Performing the dataflow
///
/// Then for each of our moveOut blocks, we visit successors recursively using a
/// BasicBlockWorklist and stop if we run out of unique blocks or if a block we
/// visit is a livenessUse block. This means that this _move has a use before an
/// init. We emit a diagnostic and exit. If a block we visit is a reinit block,
/// then we stop scanning further successors.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-kills-copyable-addresses-checker"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalOSSALifetime.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/ADT/PointerSumType.h"

using namespace swift;

static llvm::cl::opt<bool>
    DisableUnhandledMoveDiagnostic("sil-disable-unknown-moveaddr-diagnostic");

//===----------------------------------------------------------------------===//
//                            Diagnostic Utilities
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

//===----------------------------------------------------------------------===//
//                               Use Gathering
//===----------------------------------------------------------------------===//

namespace {

struct UseState {
  SmallSetVector<MarkMoveAddrInst *, 8> moves;
  SmallSetVector<SILInstruction *, 8> inits;
  SmallSetVector<SILInstruction *, 8> livenessUses;

  void clear() {
    moves.clear();
    inits.clear();
    livenessUses.clear();
  }
};

} // namespace

namespace {

/// Visit all uses of an access path using accessPathUse
struct GatherUseVisitor : public AccessUseVisitor {
  UseState &useState;
  GatherUseVisitor(UseState &useState)
      : AccessUseVisitor(AccessUseType::Overlapping,
                         NestedAccessType::IgnoreAccessBegin),
        useState(useState) {}

  bool visitUse(Operand *op, AccessUseType useTy) override;
  void clear() { useState.clear(); }
};

} // end anonymous namespace

// Filter out recognized uses that do not write to memory.
//
// TODO: Ensure that all of the conditional-write logic below is encapsulated in
// mayWriteToMemory and just call that instead. Possibly add additional
// verification that visitAccessPathUses recognizes all instructions that may
// propagate pointers (even though they don't write).
bool GatherUseVisitor::visitUse(Operand *op, AccessUseType useTy) {
  // If this operand is for a dependent type, then it does not actually access
  // the operand's address value. It only uses the metatype defined by the
  // operation (e.g. open_existential).
  if (op->isTypeDependent()) {
    return true;
  }

  // If we have a move from src, this is a mark_move we want to visit.
  if (auto *move = dyn_cast<MarkMoveAddrInst>(op->getUser())) {
    if (move->getSrc() == op->get()) {
      LLVM_DEBUG(llvm::dbgs() << "Found move: " << *move);
      useState.moves.insert(move);
      return true;
    }
  }

  if (memInstMustInitialize(op)) {
    LLVM_DEBUG(llvm::dbgs() << "Found init: " << *op->getUser());
    useState.inits.insert(op->getUser());
    return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "Found liveness use: " << *op->getUser());
  useState.livenessUses.insert(op->getUser());
  return true;
}

//===----------------------------------------------------------------------===//
//                                  Dataflow
//===----------------------------------------------------------------------===//

namespace {

class DownwardScanForMoveResult {
  using IntTy = llvm::PointerEmbeddedInt<bool, 1>;
  PointerUnion<SILInstruction *, IntTy> result;

public:
  DownwardScanForMoveResult(SILInstruction *userForDiagnostic)
      : result(userForDiagnostic) {}
  DownwardScanForMoveResult(bool isLiveOut) : result(isLiveOut) {}

  /// If we are supposed to emit a diagnostic,
  SILInstruction *getUserForDiagnostic() const {
    return result.dyn_cast<SILInstruction *>();
  }

  bool isLiveOut() {
    // We are taking advantage of PointerUnion dyn_cast being false both if we
    // have the diagnostic user or if isLiveOut is set to false.
    return result.dyn_cast<IntTy>();
  }
};

} // namespace

static DownwardScanForMoveResult downwardScanForMoveOut(MarkMoveAddrInst *mvi,
                                                        UseState &useState) {
  auto *mviParent = mvi->getParent();
  // Forward scan looking for uses or reinits.
  for (auto ii = mvi->getIterator(), ie = mviParent->end(); ii != ie; ++ii) {
    auto *next = &*ii;
    if (useState.livenessUses.count(next)) {
      // Emit a diagnostic error and process the next value...
      return DownwardScanForMoveResult(next);
    }

    if (useState.inits.count(next)) {
      // In this case, we need to skip to the next mvi.
      return DownwardScanForMoveResult(false /*is move out*/);
    }
  }

  // We are move out!
  return DownwardScanForMoveResult(true /*is move out*/);
}

static bool upwardScanForUseOut(SILInstruction *inst, UseState &useState) {
  auto *instParent = inst->getParent();
  for (auto ii = std::next(inst->getReverseIterator()), ie = instParent->rend();
       ii != ie; ++ii) {
    auto *next = &*ii;
    if (useState.livenessUses.contains(next))
      return false;
    if (auto *mvi = dyn_cast<MarkMoveAddrInst>(next))
      if (useState.moves.contains(mvi))
        return false;
    if (useState.inits.contains(next))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
//                              Address Checker
//===----------------------------------------------------------------------===//

namespace {

struct MoveKillsCopyableAddressesObjectChecker {
  SmallSetVector<SILValue, 32> addressesToCheck;
  SILFunction *fn;
  UseState useState;
  GatherUseVisitor visitor;
  llvm::DenseMap<SILBasicBlock *, SILInstruction *> useBlocks;
  llvm::DenseSet<SILBasicBlock *> initBlocks;
  SmallVector<MarkMoveAddrInst *, 8> movesToDataflow;

  MoveKillsCopyableAddressesObjectChecker(SILFunction *fn)
      : fn(fn), useState(), visitor(useState) {}
  bool gatherStateFromMoves(SILValue address);
  void performDataflow(SILValue address);

  bool check();

  void emitDiagnosticForMove(SILValue borrowedValue,
                             StringRef borrowedValueName, MoveValueInst *mvi);

  ASTContext &getASTContext() const { return fn->getASTContext(); }
};

} // namespace

static SourceLoc getSourceLocFromValue(SILValue value) {
  if (auto *defInst = value->getDefiningInstruction())
    return defInst->getLoc().getSourceLoc();
  if (auto *arg = dyn_cast<SILFunctionArgument>(value))
    return arg->getDecl()->getLoc();
  llvm_unreachable("Do not know how to get source loc for value?!");
}

bool MoveKillsCopyableAddressesObjectChecker::gatherStateFromMoves(
    SILValue address) {
  bool didEmitDiagnostic = false;
  for (auto *mvi : useState.moves) {
    // First scan downwards to make sure we are move out of this block. If we
    // are, mark this block.
    auto result = downwardScanForMoveOut(mvi, useState);
    if (auto *diagnosticUser = result.getUserForDiagnostic()) {
      auto diag =
          diag::sil_movekillscopyablevalue_value_consumed_more_than_once;
      StringRef name = getDebugVarName(address);
      diagnose(getASTContext(), getSourceLocFromValue(address), diag, name);
      didEmitDiagnostic = true;

      // We purposely continue to see if at least in simple cases, we can flag
      // mistakes from other moves. Since we are setting emittedDiagnostic to
      // true, we will not perform the actual dataflow due to a check after
      // the loop.
      continue;
    }

    // If we are live out, add all successors of this block to the worklist to
    // be checked.
    if (result.isLiveOut()) {
      movesToDataflow.push_back(mvi);
    }

    // Now scan up to see if mvi is also a use.
    if (upwardScanForUseOut(mvi, useState))
      useBlocks[mvi->getParent()] = mvi;
  }
  return didEmitDiagnostic;
}

void MoveKillsCopyableAddressesObjectChecker::performDataflow(
    SILValue address) {
  BasicBlockWorklist worklist(fn);
  while (!movesToDataflow.empty()) {
    auto *mvi = movesToDataflow.pop_back_val();

    for (auto *succBlock : mvi->getParent()->getSuccessorBlocks()) {
      worklist.pushIfNotVisited(succBlock);
    }

    while (auto *next = worklist.pop()) {
      auto iter = useBlocks.find(next);
      if (iter != useBlocks.end()) {
        // We found one! Emit the diagnostic and returnl
        auto &astContext = getASTContext();
        {
          auto diag =
              diag::sil_movekillscopyablevalue_value_consumed_more_than_once;
          StringRef name = getDebugVarName(address);
          diagnose(astContext, getSourceLocFromValue(address), diag, name);
        }

        {
          auto diag = diag::sil_movekillscopyablevalue_move_here;
          diagnose(astContext, mvi->getLoc().getSourceLoc(), diag);
        }

        {
          auto diag = diag::sil_movekillscopyablevalue_use_here;
          diagnose(astContext, iter->second->getLoc().getSourceLoc(), diag);
        }
        return;
      }

      // Then see if this is an init block. If so, continue.
      if (initBlocks.count(next))
        continue;

      // Otherwise, add successors if we haven't visited them to the worklist.
      for (auto *succBlock : next->getSuccessorBlocks()) {
        worklist.pushIfNotVisited(succBlock);
      }
    }
  }
}

bool MoveKillsCopyableAddressesObjectChecker::check() {
  if (addressesToCheck.empty())
    return false;

  LLVM_DEBUG(llvm::dbgs() << "Visiting Function: " << fn->getName() << "\n");
  auto addressToProcess =
      llvm::makeArrayRef(addressesToCheck.begin(), addressesToCheck.end());

  while (!addressToProcess.empty()) {
    auto address = addressToProcess.front();
    addressToProcess = addressToProcess.drop_front(1);
    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *address);

    auto accessPathWithBase = AccessPathWithBase::compute(address);
    auto accessPath = accessPathWithBase.accessPath;

    // Bail on an invalid AccessPath.
    //
    // AccessPath completeness is verified independently--it may be invalid in
    // extraordinary situations. When AccessPath is valid, we know all its uses
    // are recognizable.
    //
    // NOTE: If due to an invalid access path we fail here, we will just error
    // on the _move since the _move would not have been handled.
    if (!accessPath.isValid())
      continue;

    SWIFT_DEFER { visitor.clear(); };
    if (!visitAccessPathUses(visitor, accessPath, fn))
      continue;

    // Now initialize our data structures.
    SWIFT_DEFER {
      useBlocks.clear();
      initBlocks.clear();
      movesToDataflow.clear();
    };

    // Gather state from moves and if we emit a diagnostic, just continue. The
    // diagnostic is from a single block check rather than the dataflow. But
    // once we find something invalid rather than performing the dataflow on
    // weird state, just let the user fix the issue first.
    if (!gatherStateFromMoves(address))
      continue;

    // Go through all init uses and if we don't see any other of our uses, then
    // mark this as an "init block".
    for (auto *init : useState.inits) {
      if (upwardScanForUseOut(init, useState)) {
        initBlocks.insert(init->getParent());
      }
    }

    // Then go through all normal uses and do upwardScanForUseOut.
    for (auto *user : useState.livenessUses) {
      if (upwardScanForUseOut(user, useState))
        useBlocks[user->getParent()] = user;
    }

    // Ok, we are setup. Perform the dataflow!
    performDataflow(address);
  }

  return false;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class MoveKillsCopyableAddressesCheckerPass : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();
    auto &astContext = fn->getASTContext();

    // If we do not have experimental move only enabled, do not emit
    // diagnostics.
    if (!astContext.LangOpts.EnableExperimentalMoveOnly)
      return;

    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    MoveKillsCopyableAddressesObjectChecker checker(getFunction());

    for (auto &block : *fn) {
      for (auto &ii : block) {
        if (auto *asi = dyn_cast<AllocStackInst>(&ii)) {
          checker.addressesToCheck.insert(asi);
        }
      }
    }

    if (checker.check()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }

    // Now search through our function one last time and any move_value
    // [allows_diagnostics] that remain are ones that we did not know how to
    // check so emit a diagnostic so the user doesn't assume that they have
    // guarantees.
    //
    // TODO: Emit specific diagnostics here (e.x.: _move of global).
    if (DisableUnhandledMoveDiagnostic)
      return;
    for (auto &block : *fn) {
      for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
        auto *inst = &*ii;
        ++ii;

        if (auto *mai = dyn_cast<MarkMoveAddrInst>(inst)) {
          auto diag =
              diag::sil_movekillscopyablevalue_move_applied_to_unsupported_move;
          diagnose(astContext, mai->getLoc().getSourceLoc(), diag);

          // Now that we have emitted the error, replace the move_addr with a
          // copy_addr so that future passes never see it. We mark it as a
          // copy_addr [init].
          SILBuilderWithScope builder(mai);
          builder.createCopyAddr(mai->getLoc(), mai->getSrc(), mai->getDest(),
                                 IsNotTake, IsInitialization);
          mai->eraseFromParent();
        }
      }
    }
  }
};

} // anonymous namespace

SILTransform *swift::createMoveKillsCopyableAddressesChecker() {
  return new MoveKillsCopyableAddressesCheckerPass();
}
