//===--- ARCOpts.cpp ------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This is a file that concatenates together all of the various Loop ARC
/// optimizations so that they can run together.
///
//===----------------------------------------------------------------------===//

#include "swift/SILPasses/Utils/LoopUtils.h"

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

class ARCOpts : public SILFunctionTransform {
  /// The entry point to the transformation.
  void run() override {
    // If ARC optimizations are disabled, don't optimize anything and bail.
    if (!getOptions().EnableARCOptimizations)
      return;

    auto *F = getFunction();
    auto *LA = getAnalysis<SILLoopAnalysis>();
    auto *LI = LA->get(F);
    auto *DA = getAnalysis<DominanceAnalysis>();
    auto *DI = DA->get(F);

    // Canonicalize the loops, invalidating if we need to.
    if (canonicalizeAllLoops(DI, LI)) {
      // We preserve loop info and the dominator tree.
      DA->lockInvalidation();
      LA->lockInvalidation();
      PM->invalidateAnalysis(F, SILAnalysis::InvalidationKind::FunctionBody);
      DA->unlockInvalidation();
      LA->unlockInvalidation();
    }

    auto *AA = getAnalysis<AliasAnalysis>();
    auto *POTA = getAnalysis<PostOrderAnalysis>();
    auto *RCFI = getAnalysis<RCIdentityAnalysis>()->get(F);
    auto *LRFI = getAnalysis<LoopRegionAnalysis>()->get(F);

    SILLoopVisitorSet VisitorSet(F, LI);
    VisitorSet.addVisitor(
        std::make_unique(new LoopARCPairingContext(F, AA, LRFI, SLI, RCFI)));
    VisitorSet.run();

    if (VisitorSet.madeChange()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);
    }
  }

  StringRef getName() override { return "ARC Sequence Opts"; }
};

} // end anonymous namespace

SILTransform *swift::createARCOpts() {
  return new ARCOpts();
}
