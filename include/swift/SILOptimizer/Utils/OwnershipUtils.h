//===--- OwnershipUtils.h -------------------------------------------------===//
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
/// Ownership Utilities that rely on SILOptimizer functionality
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_OWNERSHIPUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_OWNERSHIPUTILS_H

#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILModule.h"

namespace swift {

// Defined in BasicBlockUtils.h
struct JointPostDominanceSetComputer;

struct OwnershipFixupContext {
  std::function<void(SILInstruction *)> eraseNotify;
  std::function<void(SILInstruction *)> newInstNotify;
  DeadEndBlocks &deBlocks;
  JointPostDominanceSetComputer &jointPostDomSetComputer;

  SILBasicBlock::iterator
  replaceAllUsesFixingOwnership(SingleValueInstruction *oldValue,
                                SILValue newValue);

  /// We can not RAUW all old values with new values.
  ///
  /// Namely, we do not support RAUWing values with ValueOwnershipKind::None
  /// that have uses that do not require ValueOwnershipKind::None or
  /// ValueOwnershipKind::Any.
  static bool canFixUpOwnershipForRAUW(SingleValueInstruction *oldValue,
                                       SILValue newValue) {
    auto newOwnershipKind = newValue.getOwnershipKind();

    // If our new kind is ValueOwnershipKind::None, then we are fine. We
    // trivially support that. This check also ensures that we can always
    // replace any value with a ValueOwnershipKind::None value.
    if (newOwnershipKind == OwnershipKind::None)
      return true;

    // If we are in Raw SIL, just bail at this point. We do not support
    // ownership fixups.
    if (oldValue->getModule().getStage() == SILStage::Raw)
      return false;

    // In constrast, if our old ownership kind is ValueOwnershipKind::None and
    // our new kind is not, we may need to do more work. If our old ownership
    // kind is not none, we are good to go.
    auto oldOwnershipKind = SILValue(oldValue).getOwnershipKind();
    if (oldOwnershipKind != OwnershipKind::None)
      return true;

    // Ok, we have an old ownership kind that is OwnershipKind::None and a new
    // ownership kind that is not OwnershipKind::None. In that case, for now, do
    // not perform this transform.
    return false;
  }
};

} // namespace swift

#endif
