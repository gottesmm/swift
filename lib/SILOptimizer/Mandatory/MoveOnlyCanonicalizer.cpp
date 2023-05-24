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

#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

////////////////////////////////
// MARK: Top Level Entrypoint //
////////////////////////////////

namespace {

class MoveOnlyCanonicalizer : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();

    // Only run this pass if the move only language feature is enabled.
    if (!fn->getASTContext().supportsMoveOnlyTypes())
      return;

    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    // If an earlier pass told use to not emit diagnostics for this function,
    // clean up any copies, invalidate the analysis, and return early.
    if (getFunction()->hasSemanticsAttr(semantics::NO_MOVEONLY_DIAGNOSTICS)) {
      if (cleanupNonCopyableCopiesAfterEmittingDiagnostic(getFunction()))
        invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
      return;
    }

    LLVM_DEBUG(llvm::dbgs()
               << "===> MoveOnly Checker. Visiting: " << fn->getName() << '\n');

    MoveOnlyChecker checker(
        getFunction(), getAnalysis<DominanceAnalysis>()->get(getFunction()),
        getAnalysis<PostOrderAnalysis>());

    checker.checkObjects();
    checker.checkAddresses();

    // If we did not emit any diagnostics, emit an error on any copies that
    // remain. If we emitted a diagnostic, we just want to rewrite all of the
    // non-copyable copies into explicit variants below and let the user
    // recompile.
    if (!checker.diagnosticEmitter.emittedDiagnostic()) {
      emitCheckerMissedCopyOfNonCopyableTypeErrors(getFunction(),
                                                   checker.diagnosticEmitter);
    }

    checker.madeChange |=
        cleanupNonCopyableCopiesAfterEmittingDiagnostic(getFunction());

    if (checker.madeChange)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // namespace

SILTransform *swift::createMoveOnlyCanonicalizer() {
  return new MoveOnlyCanonicalizer();
}
