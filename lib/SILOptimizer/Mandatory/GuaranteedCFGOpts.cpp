//===--- GuaranteedCFGOpts.cpp --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-guaranteed-cfg-opts"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFG.h"

using namespace swift;

namespace {

struct GuaranteedCFGOpts : SILFunctionTransform {
  void run() override {
    assert(getFunction()->getModule().getStage() == SILStage::Raw &&
           "Can only run GuaranteedCFGOpts in raw SIL");

    llvm::SmallVector<BranchInst *, 64> Worklist;
    for (auto &BB : *getFunction()) {
      auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
      if (!BI)
        continue;
      SILBasicBlock *SuccBB = BI->getDestBB();
      if (SuccBB == &BB || !SuccBB->getSinglePredecessor())
        continue;
      Worklist.push_back(BI);
    }

    bool MadeChange = !Worklist.empty();

    while (!Worklist.empty()) {
      BranchInst *BI = Worklist.pop_back_val();
      mergeBasicBlockWithSuccessor(BI, BI->getDestBB(), nullptr, nullptr);
    }

    if (MadeChange) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Everything);
    }
  }

  StringRef getName() override { return "Guaranteed CFG Opts"; }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

SILTransform *swift::createGuaranteedCFGOpts() {
  return new GuaranteedCFGOpts();
}
