//===--- MoveOnlyCanonicalization.cpp -------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// A small transformation that canonicalizes SIL before running the move
/// checker upon it.
///
/// These occur sometimes due to SILGen bouncing values back/forth through
/// memory when emitting code in library evolution mode.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "swift/Basic/Defer.h"
#include "swift/SIL/AddressWalker.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/StackList.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"

#include "MoveOnlyUtils.h"

using namespace swift;
using namespace swift::siloptimizer;

//===----------------------------------------------------------------------===//
//                             MARK: Optimization
//===----------------------------------------------------------------------===//

namespace {

struct TemporaryAllocationVisitorState {
  AllocStackInst *asi;
  Operand *initializer;

  /// We only allow for our temporary allocation to be used a single time by an
  /// instruction. SILGen generally introduces one of these temporaries
  /// specifically for a single use. Additionally, pruned liveness as an
  /// invariant only tracks one instruction at a time.
  InstructionSet userSet;

  SmallVector<std::pair<Operand *, TypeTreeLeafTypeRange>, 8> consumingUses;
  SmallVector<std::pair<Operand *, TypeTreeLeafTypeRange>, 8> livenessUses;
  SmallVector<DestroyAddrInst *, 8> destroys;
  FieldSensitiveSSAPrunedLiveRange liveness;

  TemporaryAllocationVisitorState(
      AllocStackInst *asi, SmallVectorImpl<SILBasicBlock *> &discoveredBlocks)
      : asi(asi), initializer(nullptr), userSet(asi->getFunction()),
        liveness(asi->getFunction(), &discoveredBlocks) {
    liveness.init(asi);
    liveness.initializeDef(asi, TypeTreeLeafTypeRange(asi));
  }

  // Defined in top level entrypoint below.
  bool process();

  bool addConsumingUse(Operand *op) {
    if (!userSet.insert(op->getUser()))
      return false;
    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), asi);
    if (!leafRange)
      return false;
    LLVM_DEBUG(llvm::dbgs() << "Found consuming use: " << *op->getUser());
    liveness.updateForUse(op->getUser(), *leafRange, true);
    consumingUses.emplace_back(op, *leafRange);
    return true;
  }

  bool addLivenessUse(Operand *op) {
    if (!userSet.insert(op->getUser()))
      return false;
    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), asi);
    if (!leafRange)
      return false;
    LLVM_DEBUG(llvm::dbgs() << "Found liveness use: " << *op->getUser());
    liveness.updateForUse(op->getUser(), *leafRange, false);
    livenessUses.emplace_back(op, *leafRange);
    return true;
  }

  bool addDestroyAddrUse(DestroyAddrInst *dai) {
    auto leafRange = TypeTreeLeafTypeRange::get(dai->getOperand(), asi);
    // We require all destroy_addr to be on the entire value.
    if (!leafRange || *leafRange != TypeTreeLeafTypeRange(asi))
      return false;
    LLVM_DEBUG(llvm::dbgs() << "Found destroy_addr use: " << *dai);
    destroys.push_back(dai);
    return true;
  }

  bool setInitializer(Operand *op) {
    if (initializer) {
      LLVM_DEBUG(llvm::dbgs() << "Already have initializer!\n");
      return false;
    }
    auto leafRange = TypeTreeLeafTypeRange::get(op->get(), asi);
    if (!leafRange || *leafRange != TypeTreeLeafTypeRange(asi))
      return false;
    LLVM_DEBUG(llvm::dbgs() << "Found init: " << *op->getUser());
    initializer = op;
    return true;
  }

private:
  bool processBorrowedAllocation();
  bool processBorrowedAllocationStoreInst(StoreInst *si);
  bool processBorrowedAllocationCopyAddrInst(CopyAddrInst *si);
  bool isLiveWithinBorrowedValueLifetime(BorrowedValue bv);
};

} // namespace

//===----------------------------------------------------------------------===//
//                        MARK: Borrowed Optimization
//===----------------------------------------------------------------------===//

bool TemporaryAllocationVisitorState::isLiveWithinBorrowedValueLifetime(
    BorrowedValue bv) {
  // If we have function wide scope, then we can optimize.
  if (!bv.isLocalScope())
    return true;

  // Otherwise, grab the end lifetime of this bv and make sure that our
  // liveness has that all of the end scope uses of the borrowed value are not
  // within the boundary of our liveness.
  return bv.visitExtendedScopeEndingUses([&](Operand *use) -> bool {
    return !liveness.isWithinBoundary(use->getUser(),
                                      TypeTreeLeafTypeRange(asi));
  });
}

bool TemporaryAllocationVisitorState::processBorrowedAllocationStoreInst(
    StoreInst *si) {
  auto *cvi = dyn_cast<CopyValueInst>(si->getSrc());
  if (!cvi || !hasOneNonDebugUse(cvi))
    return false;

  SILValue operand = cvi->getOperand();
  if (operand->getOwnershipKind() != OwnershipKind::Guaranteed)
    return false;

  // Grab the borrow introducer. We only want one.
  BorrowedValue bv;
  if (!visitBorrowIntroducers(operand, [&](SILValue introducer) -> bool {
        if (bv)
          return false;
        bv = BorrowedValue(introducer);
        return true;
      })) {
    return false;
  }

  if (!isLiveWithinBorrowedValueLifetime(bv)) {
    return false;
  }

  // Ok, we can convert this to a store_borrow instead of a copy_value
  // + store [init].
  InstructionDeleter deleter;
  StoreBorrowInst *sbi;
  {
    SILBuilderWithScope builder(si);
    sbi = builder.createStoreBorrow(si->getLoc(), operand, si->getDest());
  }
  for (auto *dai : destroys) {
    SILBuilderWithScope borrowBuilder(dai);
    borrowBuilder.createEndBorrow(dai->getLoc(), sbi);
    deleter.forceDelete(dai);
  }
  deleter.forceDelete(si);
  deleter.forceDeleteWithUsers(cvi);

  // Now go through the uses of our alloc_stack and any that aren't a
  // dealloc_stack change to use sbi instead.
  //
  // NOTE: We cannot use our found liveness uses since they may have a path of
  // GEPs in the def-use graph in between them and the allocation. Instead, we
  // need to change the immediate uses of alloc_stack to be on sbi.
  for (auto ui = asi->use_begin(), ue = asi->use_end(); ui != ue;) {
    auto *use = *ui;
    ++ui;

    if (isa<DeallocStackInst>(use->getUser()))
      continue;
    if (use->getUser() == sbi)
      continue;
    use->set(sbi);
  }
  return true;
}

bool TemporaryAllocationVisitorState::processBorrowedAllocation() {
  // See if our initializer is a store inst of something with a copy_value from
  // a guaranteed value. In such a case if the guaranteed value lasts as long as
  // our allocation, try to change the allocation to use a store_borrow instead.
  if (auto *si = dyn_cast<StoreInst>(initializer->getUser())) {
    return processBorrowedAllocationStoreInst(si);
  }

  return false;
}

bool TemporaryAllocationVisitorState::process() {
  // If we don't have an initializer, we need to fail.
  if (!initializer)
    return false;

  // If we do not have any consuming uses, then we may have an allocation that
  // is functioning as a borrow.
  if (consumingUses.empty()) {
    return processBorrowedAllocation();
  }

  return false;
}

//===----------------------------------------------------------------------===//
//                           MARK: Address Analysis
//===----------------------------------------------------------------------===//

namespace {

/// An early transform that we run to convert any load_borrow that are copied
/// directly or that have any subelement that is copied to a load [copy]. This
/// lets the rest of the optimization handle these as appropriate.
struct TemporaryAllocationVisitor final : public TransitiveAddressWalker {
  SILFunction *fn;
  TemporaryAllocationVisitorState &state;

  TemporaryAllocationVisitor(SILFunction *fn,
                             TemporaryAllocationVisitorState &state)
      : fn(fn), state(state) {}

  bool visitUse(Operand *op) override {
    LLVM_DEBUG(llvm::dbgs() << "TemporaryAllocationVisitor visiting ";
               llvm::dbgs() << " User: " << *op->getUser());

    if (op->isTypeDependent())
      return true;

    // Skip dealloc stack inst.
    if (isa<DeallocStackInst>(op->getUser()))
      return true;

    if (auto *dai = dyn_cast<DestroyAddrInst>(op->getUser())) {
      return state.addDestroyAddrUse(dai);
    }

    if (moveonlyutils::memInstMustInitialize(op)) {
      return state.setInitializer(op);
    }

    if (moveonlyutils::memInstMustConsume(op)) {
      return state.addConsumingUse(op);
    }

    if (moveonlyutils::memInstMustReinitialize(op)) {
      return state.addLivenessUse(op);
    }

    // Otherwise, we have a liveness use.
    if (moveonlyutils::memInstOnlyRequiresLiveness(op)) {
      return state.addLivenessUse(op);
    }

    // Otherwise, we have something we don't understand... bail.
    return false;
  }
};

} // namespace

//===----------------------------------------------------------------------===//
//                         MARK: Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class MoveOnlyCanonicalizer : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();

    if (!fn->getASTContext().supportsMoveOnlyTypes())
      return;

    if (fn->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    LLVM_DEBUG(llvm::dbgs() << ">>> Running MoveOnlyCanonicalizer on: "
                            << fn->getName() << '\n');
    StackList<AllocStackInst *> targets(fn);

    for (auto &bb : *getFunction()) {
      for (auto ii = bb.begin(), ie = bb.end(); ii != ie; ++ii) {
        if (auto *asi = dyn_cast<AllocStackInst>(&*ii)) {
          if (!asi->isLexical() && asi->getType().isMoveOnly()) {
            LLVM_DEBUG(llvm::dbgs() << "Found target:" << *asi);
            targets.push_back(asi);
          }
        }
      }
    }

    bool madeChange = false;
    SmallVector<SILBasicBlock *, 32> discoveredBlocks;
    while (!targets.empty()) {
      SWIFT_DEFER { discoveredBlocks.clear(); };
      auto *asi = targets.pop_back_val();
      TemporaryAllocationVisitorState state(asi, discoveredBlocks);
      TemporaryAllocationVisitor visitor(fn, state);
      std::move(visitor).walk(asi);
      madeChange |= state.process();
    }

    if (madeChange) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // namespace

SILTransform *swift::createMoveOnlyCanonicalization() {
  return new MoveOnlyCanonicalizer();
}
