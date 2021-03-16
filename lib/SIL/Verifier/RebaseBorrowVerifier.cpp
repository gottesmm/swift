//===--- RebaseBorrowVerifier.cpp -----------------------------------------===//
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

#include "RebaseBorrowVerifierPrivate.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILValue.h"

using namespace swift;
using namespace swift::silverifier;

bool RebaseBorrowVerifier::verifyReborrow(
    BorrowingOperand reborrowBorrowingOperand, SILValue origBaseValue) {
  // Make sure to always reset our new base to original bases frozen multimap.
  SWIFT_DEFER { newBaseToOriginalBase.reset(); };

  // OrigBaseValue may be owned or guaranteed. Initial scope operand is the
  // borrowing operand of origBaseValue that caused it to be borrowed.

  // First we need to gather up all base values and any rebase_borrows from that
  // value.
  {
    SmallVector<Operand *, 32> scratchSpace;

    findTransitiveReborrowBaseValuePairs(
        reborrowBorrowingOperand, origBaseValue,
        [&](SILPhiArgument *reborrow, SILValue baseValue) {
          findTransitiveGuaranteedUses(reborrow, scratchSpace);
          while (!scratchSpace.empty()) {
            auto *nonForwardingUse = scratchSpace.pop_back_val();
            auto *rebaseBorrow =
                dyn_cast<RebaseBorrowInst>(nonForwardingUse->getUser());
            if (!rebaseBorrow)
              return;
            newBaseToOriginalBase.insert(baseValue,
                                         rebaseBorrow->getBaseOperand());
          }
        });
  }

  // Then finalize our frozen multi-map so that we can get our stable_sorted,
  // ranges.
  newBaseToOriginalBase.setFrozen();

  for (auto pair : newBaseToOriginalBase.getRange()) {
    SILValue newBase = pair.first;
    assert(newBase->getType().isObject() &&
           "Can we support rebasing on an address location?");
    BorrowedValue newBaseBV(newBase);
    for (SILValue oldBase : pair.second) {
      // If our old base was an object base, just make sure it's entire extended
      // lifetime is within the new base's lifetime.
      if (oldBase->getType().isObject()) {
        BorrowedValue bv(oldBase);
        assert(bool(bv));

        SWIFT_DEFER {
          oldBaseUseScratchSpace.clear();
          scratchSpace.clear();
        };
        bv.visitExtendedLocalScopeEndingUses([&](Operand *op) {
          oldBaseUseScratchSpace.push_back(op);
          return true;
        });
        if (!newBaseBV.areUsesWithinScope(oldBaseUseScratchSpace, scratchSpace,
                                          deadEndBlocks)) {
          return false;
        }
        continue;
      }

      // Otherwise, we have an old base that is an address type. That means that
      // we are rebasing a load_borrow base. We only support doing this if the
      // underlying memory base is an interior pointer than was projected out of
      // guaranteed value which acts as the real base.
      SILValue addProj =
          getUnderlyingObjectStoppingAtObjectToAddrProjections(oldBase);
      auto intPtrOperand = InteriorPointerOperand::inferFromResult(addProj);
      if (!intPtrOperand)
        return false;

      SWIFT_DEFER {
        oldBaseUseScratchSpace.clear();
        scratchSpace.clear();
      };
      // TODO: Make this the extended use?
      intPtrOperand.visitBaseValueScopeEndingUses([&](Operand *use) {
        oldBaseUseScratchSpace.push_back(use);
        return true;
      });
      if (!newBaseBV.areUsesWithinScope(oldBaseUseScratchSpace, scratchSpace,
                                        deadEndBlocks)) {
        return false;
      }
    }
  }

  // We are valid!
  return true;
}

bool RebaseBorrowVerifier::verifyBaseValue(BorrowedValue initialBV,
                                           RebaseBorrowInst *rbi) {
  // For now if our borrowed value doesn't have a base local to the current
  // function, just return true.
  if (!initialBV.isLocalScope())
    return true;

  // First see if we can find a single local BorrowingOperand for our initial
  // BV. If so, we know that it must be an object since BorrowingOperands
  // currently are never adddresses.
  if (BorrowingOperand oldBase = initialBV.getSingleLocalBorrowingOperand()) {
    assert(oldBase->get().isObject());

    // Then see if our old borrowing operand is the same as our base operand. If
    // so, we are good!
    if (oldBase->get() == rbi->getBaseOperand())
      return true;

    // Otherwise, make sure that initialBV is completely within the scope of
    // oldBase.
  }

  // If our old base is the same as our new base, we are done.
  auto newBase = rbi->getBaseOperand();
  if (newBase == oldBase)
    return true;

  // Get the borrowed value of our new base...
  BorrowedValue newBaseBV(newBase);
  assert(bool(newBaseBV) && "Base operand must be a borrowed value");

  // Then if our old base was an object base, just make sure it's entire
  // extended lifetime is within the new base's lifetime.
  if (oldBase->getType().isObject()) {
    SWIFT_DEFER {
      oldBaseUseScratchSpace.clear();
      scratchSpace.clear();
    };
    initialBV.visitExtendedLocalScopeEndingUses([&](Operand *op) {
      oldBaseUseScratchSpace.push_back(op);
      return true;
    });
    if (!newBaseBV.areUsesWithinScope(oldBaseUseScratchSpace, scratchSpace,
                                      deadEndBlocks)) {
      return false;
    }
    return true;
  }

  // Otherwise, we have an old base that is an address type. That means that
  // we are rebasing a load_borrow base. We only support doing this if the
  // underlying memory base is an interior pointer than was projected out of
  // guaranteed value which acts as the real base.
  SILValue addrProj =
      getUnderlyingObjectStoppingAtObjectToAddrProjections(*initialBV);
  auto intPtrOperand = InteriorPointerOperand::inferFromResult(addrProj);
  if (!intPtrOperand)
    return false;

  SWIFT_DEFER {
    oldBaseUseScratchSpace.clear();
    scratchSpace.clear();
  };
  // TODO: Make this the extended use?
  intPtrOperand.visitBaseValueScopeEndingUses([&](Operand *use) {
    oldBaseUseScratchSpace.push_back(use);
    return true;
  });
  if (!newBaseBV.areUsesWithinScope(oldBaseUseScratchSpace, scratchSpace,
                                    deadEndBlocks)) {
    return false;
  }

  return true;
}
