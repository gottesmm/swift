//===--- RebaseBorrowVerifierPrivate.h ------------------------------------===//
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

#ifndef SWIFT_SIL_VERIFIER_REBASEBORROWVERIFIERPRIVATE_H
#define SWIFT_SIL_VERIFIER_REBASEBORROWVERIFIERPRIVATE_H

#include "LinearLifetimeCheckerPrivate.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILValue.h"

namespace swift {
namespace silverifier {

class RebaseBorrowVerifier {
  /// A cache of dead-end basic blocks that we use to determine if we can
  /// ignore "leaks".
  DeadEndBlocks &deadEndBlocks;

  /// The builder that the checker uses to emit error messages, crash if asked
  /// for, or supply back interesting info to the caller.
  LinearLifetimeChecker::ErrorBuilder errorBuilder;

  /// A cache that we use when processing that maps any rebase_borrow new base
  /// instructions to the current base that we are processing.
  SmallFrozenMultiMap<SILValue, SILValue, 8> newBaseToOriginalBase;

  /// Scratch space to store uses of old bases when testing.
  ///
  /// NOTE: Must be cleared in between uses!
  SmallVector<Operand *, 8> oldBaseUseScratchSpace;

  /// Additional scratch space used when checking if an old base's uses are
  /// within the lifetime of a new base value.
  ///
  /// NOTE: Must be cleared in between uses!
  SmallVector<Operand *, 8> scratchSpace;

public:
  RebaseBorrowVerifier(DeadEndBlocks &deadEndBlocks,
                       LinearLifetimeChecker::ErrorBuilder errorBuilder)
      : deadEndBlocks(deadEndBlocks), errorBuilder(errorBuilder) {}

  /// Given a borrowing operand \p initialScopeOperand with an owned or
  /// gauranteed base value \p origBaseValue, validate that if \p
  /// initialScopeOperand is rebased onto a new base newBase, that newBase is
  /// also a derived guaranteed base of \p origBaseValue.
  bool verifyReborrow(BorrowingOperand reborrowBorrowingOperand,
                      SILValue origBaseValue);

  bool verifyBaseValue(BorrowedValue origBaseValue, RebaseBorrowInst *rbi);
};

} // namespace silverifier
} // namespace swift

#endif
