//===--- MoveOnlyCanonicalizer.cpp ----------------------------------------===//
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

#include "swift/SIL/StackList.h"
#include "swift/SIL/AddressWalker.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/Basic/Defer.h"

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
      : asi(asi), initializer(nullptr),
        userSet(asi->getFunction()),
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
    if (*leafRange != TypeTreeLeafTypeRange(asi))
      return false;
    destroys.push_back(dai);
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

bool TemporaryAllocationVisitorState::processBorrowedAllocationStoreInst(StoreInst *si) {
  auto *cvi = dyn_cast<CopyValueInst>(initializer->get());
  if (!cvi)
    return false;
  if (cvi->getOperand()->getOwnershipKind() != OwnershipKind::Guaranteed)
    return false;

  // Grab the borrow introducer. We only want one.
  BorrowedValue bv;
  if (!visitBorrowIntroducers(cvi->getOperand(),
                             [&](SILValue introducer) -> bool {
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
  StoreBorrowInst *sbi;
  {
    SILBuilderWithScope builder(si);
    sbi = builder.createStoreBorrow(si->getLoc(), si->getSrc(), si->getDest());
  }
  for (auto *dai : destroys) {
    SILBuilderWithScope borrowBuilder(dai);
    borrowBuilder.createEndBorrow(dai->getLoc(), sbi);
    dai->eraseFromParent();
  }
  cvi->eraseFromParent();
  si->eraseFromParent();
  return true;
}

static SILValue lookThroughProjections(SILValue value) {
  while (true) {
    if (auto *mmci = dyn_cast<MarkMustCheckInst>(value)) {
      value = mmci->getOperand();
      continue;
    }

    SILValue newValue = lookThroughAddressToAddressProjections(value);
    if (newValue != value) {
      value = newValue;
      continue;
    }

    return value;
  }
}

bool TemporaryAllocationVisitorState::processBorrowedAllocationCopyAddrInst(CopyAddrInst *cai) {
  // We cann't handle take of src.
  if (cai->isTakeOfSrc())
    return false;
  assert(cai->isInitializationOfDest());

  SILValue underlyingObject =
    lookThroughProjections(cai->getSrc());

  // If we have a function argument that is in_guaranteed, then we know that we
  // can eliminate this allocation and just use the guaranteed parameter.
  if (auto *fArg = dyn_cast<SILFunctionArgument>(underlyingObject)) {
    if (fArg->getKnownParameterInfo().isGuaranteed()) {
      SILValue src = cai->getSrc();
      cai->eraseFromParent();
      
      // Rewrite all uses onto cai->getSrc().
      for (auto ui = asi->use_begin(), ue = asi->use_end(); ui != ue;) {
        auto *user = ui->getUser();
        unsigned operandNumber = ui->getOperandNumber();
        ++ui;

        if (auto *dai = dyn_cast<DestroyAddrInst>(user)) {
          dai->eraseFromParent();
          continue;
        }

        if (auto *dsi = dyn_cast<DeallocStackInst>(user)) {
          dsi->eraseFromParent();
          continue;
        }

        user->setOperand(operandNumber, src);
      }

      asi->eraseFromParent();
      return true;
    }
  }

  return false;
}

bool TemporaryAllocationVisitorState::processBorrowedAllocation() {
  // See if our initializer is a store inst of something with a copy_value from
  // a guaranteed value. In such a case if the guaranteed value lasts as long as
  // our allocation, try to change the allocation to use a store_borrow instead.
  if (auto *si = dyn_cast<StoreInst>(initializer->getUser())) {
    return processBorrowedAllocationStoreInst(si);
  }

  if (auto *cai = dyn_cast<CopyAddrInst>(initializer->getUser())) {
    return processBorrowedAllocationCopyAddrInst(cai);
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
                             TemporaryAllocationVisitorState &state) : fn(fn), state(state) {}

  bool visitUse(Operand *op) override {
    LLVM_DEBUG(llvm::dbgs() << "TemporaryAllocationVisitor visiting ";
               llvm::dbgs() << " User: " << *op->getUser());

    if (op->isTypeDependent())
      return true;

    // Skip dealloc stack inst.
    if (isa<DeallocStackInst>(op->getUser()))
      return true;

    if (auto *dai = dyn_cast<DestroyAddrInst>(op->getUser())) {
      if (!state.addDestroyAddrUse(dai))
        return false;
      return true;
    }

    if (moveonlyutils::memInstMustInitialize(op)) {
      if (state.initializer) {
        LLVM_DEBUG(llvm::dbgs() << "Already have initializer!\n");
        return false;
      }
      state.initializer = op;
      return true;
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

}

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

    StackList<AllocStackInst *> targets(fn);

    for (auto &bb : *getFunction()) {
      for (auto ii = bb.begin(), ie = bb.end(); ii != ie; ++ii) {
        if (auto *asi = dyn_cast<AllocStackInst>(&*ii)) {
          if (!asi->isLexical() && asi->getType().isMoveOnly()) {
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

SILTransform *swift::createMoveOnlyCanonicalizer() {
  return new MoveOnlyCanonicalizer();
}
