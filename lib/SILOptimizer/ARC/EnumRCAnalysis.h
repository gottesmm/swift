//===--- EnumRCAnalysis.h -------------------------------------------------===//
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

#ifndef SWIFT_SILANALYSIS_ENUMTAGANALYSIS_H
#define SWIFT_SILANALYSIS_ENUMTAGANALYSIS_H

#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManger/PassManager.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/SILFunction.h"

namespace swift {

class PostOrderFunctionInfo;
class PostOrderAnalysis;
class DominanceAnalysis;
class AliasAnalysis;

class EnumRCFunctionInfo {
  class EnumRCFunctionInfoPImpl;

  EnumRCFunctionInfoPImpl &asPImpl() {
    return reinterpret_cast<EnumRCFunctionInfoPImpl &>(*this);
  }
  const EnumRCFunctionInfoPImpl &asPImpl() const {
    return reinterpret_cast<const EnumRCFunctionInfoPImpl &>(*this);
  }

public:
  static EnumRCFunctionInfo *create(SILFunction *F, PostOrderFunctionInfo *PO);
  ~EnumRCFunctionInfo();

  /// Returns true if we know that V either does not have a payload at BB or if
  /// V does have a payload, but the payload does not require reference
  /// counting. Returns false otherwise.
  bool doesEnumNeedRCAtBB(SILBasicBlock *BB, SILValue V) const;
};

class EnumRCAnalysis : public FunctionAnalysisBase<EnumRCFunctionInfo> {
  PostOrderAnalysis *POA;

public:
  EnumRCAnalysis(SILPassManager *PM)
    : FunctionAnalysisBase<EnumRCFunctionInfo>(AnalysisKind::EnumRC),
      POA(PM->getAnalysis<PostOrderAnalysis>()) {}

  static bool classof(const SILAnalysis *S) {
    return S->getKind() == AnalysisKind::EnumRC;
  }

  EnumRCFunctionInfo *newFunctionAnalysis(SILFunction *F) override;

  virtual bool shouldInvalidate(SILAnalysis::PreserveKind K) override {
    bool branchesPreserved = K & PreserveKind::Branches;
    return !branchesPreserved;
  }
};

} // end swift namespace

#endif
