//===--- BorrowScopeOpts.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Optimizations that attempt to simplify and or eliminate borrow scopes. Today
/// we only eliminate scopes, but we could also eliminate redundant scopes by
/// converting struct_extract operations to use destructure operations.
///
//===----------------------------------------------------------------------===//

#include "Context.h"
#include "SemanticARCOptVisitor.h"
#include "swift/SIL/DebugUtils.h"

using namespace swift;
using namespace swift::semanticarc;

bool SemanticARCOptVisitor::visitBeginBorrowInst(BeginBorrowInst *bbi) {
  // Quickly check if we are supposed to perform this transformation.
  if (!ctx.shouldPerform(ARCTransformKind::RedundantBorrowScopeElimPeephole))
    return false;

  auto kind = bbi->getOperand().getOwnershipKind();
  SmallVector<EndBorrowInst *, 16> endBorrows;
  for (auto *op : bbi->getUses()) {
    if (!op->isLifetimeEnding()) {
      // Make sure that this operand can accept our arguments kind.
      if (op->canAcceptKind(kind))
        continue;
      return false;
    }

    // Otherwise, this borrow is being consumed. See if our consuming inst is an
    // end_borrow. If it isn't, then return false, this scope is
    // needed. Otherwise, add the end_borrow to our list of end borrows.
    auto *ebi = dyn_cast<EndBorrowInst>(op->getUser());
    if (!ebi) {
      return false;
    }
    endBorrows.push_back(ebi);
  }

  // At this point, we know that the begin_borrow's operand can be
  // used as an argument to all non-end borrow uses. Eliminate the
  // begin borrow and end borrows.
  while (!endBorrows.empty()) {
    auto *ebi = endBorrows.pop_back_val();
    eraseInstruction(ebi);
  }

  eraseAndRAUWSingleValueInstruction(bbi, bbi->getOperand());
  return true;
}

//===----------------------------------------------------------------------===//
//                    Redundant Guaranteed Phi Elimination
//===----------------------------------------------------------------------===//

bool SemanticARCOptVisitor::visitSILPhiArgument(SILPhiArgument *arg) {
  if (ctx.onlyGuaranteedOpts)
    return false;

  // Quickly check if we are supposed to perform this transformation.
  if (!ctx.shouldPerform(ARCTransformKind::GuaranteedPhiArgElimPeephole))
    return false;

  // We only want to do this if we have a phi argument with guaranteed
  // ownership.
  if (!arg->isPhiArgument() ||
      arg->getOwnershipKind() != OwnershipKind::Guaranteed)
    return false;

  // See real quick if we only have debug uses and end_borrow uses. If so, we
  // can mark this argument as dead by passing in an undef and puting an
  // end_borrow on each incoming value arg. We eliminate such arguments after we
  // finish optimizing.
  //
  // NOTE: We do not need to worry about invalidating operands in our
  // joinedOwnedIntroducerToConsumedOperands since we represent them as a
  // SILBasicBlock *, terminator operand number. We do not remove arguments
  // ourselves, but instead change their incoming value to be undef so we can
  // cleanup later.
  for (auto *use : getNonDebugUses(arg)) {
    if (isa<EndBorrowInst>(use->getUser()))
      continue;
    return false;
  }

  arg->visitIncomingPhiOperands([&](Operand *op) {
    // If we have a SILUndef as our user, just continue. We don't need to insert
    // an end_borrow.
    if (isa<SILUndef>(op->get()))
      return true;
    SILBuilderWithScope builder(op->getUser());
    builder.createEndBorrow(op->getUser()->getLoc(), op->get());
    op->set(SILUndef::get(op->get()->getType(), *arg->getFunction()));
    return true;
  });

  // Now that arg is dead, mark its ownership as OwnershipKind::None and add it
  // to the dead guaranteed arg array for us to delete after we finish
  // processing.
  arg->setOwnershipKind(OwnershipKind::None);
  ctx.deadGuaranteedArgs.push_back(arg);
  return true;
}
