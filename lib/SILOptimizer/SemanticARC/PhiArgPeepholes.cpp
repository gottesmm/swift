//===--- PhiArgPeepholes.cpp ----------------------------------------------===//
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
///
/// \file
///
/// Semantic ARC optimizations on phi args. Meant to be small peepholes.
///
//===----------------------------------------------------------------------===//

#include "SemanticARCOptVisitor.h"

#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILUndef.h"

using namespace swift;
using namespace swift::semanticarc;

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

bool SemanticARCOptVisitor::visitSILPhiArgument(SILPhiArgument *arg) {
  if (ctx.onlyGuaranteedOpts)
    return false;

  // If we are not supposed to perform this transform, bail.
  if (!ctx.shouldPerform(ARCTransformKind::PhiArgPeepholes))
    return false;

  auto argKind = arg->getOwnershipKind();
  if (argKind == OwnershipKind::None || argKind == OwnershipKind::Unowned) {
    return false;
  }

  // Ok, we have an owned or guaranteed argument. Lets see if this argument only
  // has uses that are debug value instructions or end scope instructions
  // (end_borrow, destroy_value). In such a case, we can eliminate the phi arg
  // by hoisting the end_scope instruction into our predecessors on the relevant
  // incoming values and then change the br to take undef instead of the
  // incoming value.
  //
  // DISCUSSION: In SIL, incoming values are stored in branch instructions and
  // by eliminating our phi argument, we will have to invalidate all of our
  // incoming branch instructions. We want this routine to not touch branches,
  // so we use this rebalancing approach and leave eliminating the actual phi
  // arg to other passes.
  for (auto *use : getNonDebugUses(arg)) {
    auto *user = use->getUser();
    if (!isa<EndBorrowInst>(user) && !isa<DestroyValueInst>(user))
      return false;
  }

  // Then gather all of our incoming value operands. If we fail to do so, we
  // can't optimize.
  SmallVector<Operand *, 8> incomingValueOperands;
  if (!arg->getIncomingPhiOperands(incomingValueOperands))
    return false;

  // Ok, we can optimize! First delete all of our arg's uses.
  while (!arg->use_empty()) {
    auto *frontUse = *arg->use_begin();
    eraseInstruction(frontUse->getUser());
  }

  // Then for each incoming value operand...
  for (auto *use : incomingValueOperands) {
    // Create an end scope instruction on its value at its user. Its user will
    // be the relevant terminator instruction, so we will insert the end scope
    // right at that point.
    auto *user = use->getUser();
    if (argKind == OwnershipKind::Owned) {
      SILBuilderWithScope(user).emitDestroyOperation(user->getLoc(),
                                                     use->get());
    } else {
      assert(argKind == OwnershipKind::Guaranteed);
      SILBuilderWithScope(user).emitEndBorrowOperation(user->getLoc(),
                                                       use->get());
    }

    // And set the operand to instead take undef.
    use->set(SILUndef::get(use->get()->getType(), *user->getFunction()));
  }

  // Finally, set our argument to have OwnershipKind::None. Some other pass will
  // trivially eliminate it when we are eliminating other branches.
  arg->setOwnershipKind(OwnershipKind::None);

  return true;
}
