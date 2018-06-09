//===---- GuaranteedTupleScalarizer.cpp - Select access enforcement -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Definite Initialization (DI) and Predictable Memory Opts (PMO) both are not
/// sensitive to tuples so we used to expand memory operations on tuples while
/// we perform both operations. This was an unnecessary merging of passes. Today
/// instead, we use this pass to expand those tuple typed
/// loads/stores/assigns/copy_addr and runs before both of those optimizations
/// to ensure that we can maximize our analysis/optimization opportunities.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "guaranteed-tuple-scalarizer"

using namespace swift;

//===----------------------------------------------------------------------===//
//                          Scalarization Logic
//===----------------------------------------------------------------------===//

/// Given a pointer to a tuple type, compute the addresses of each element and
/// add them to the ElementAddrs vector.
static void
getScalarizedElementAddresses(SILValue Pointer, SILBuilder &B, SILLocation Loc,
                              SmallVectorImpl<SILValue> &ElementAddrs) {
  TupleType *TT = Pointer->getType().castTo<TupleType>();
  for (auto Index : indices(TT->getElements())) {
    ElementAddrs.push_back(B.createTupleElementAddr(Loc, Pointer, Index));
  }
}

/// Given an RValue of aggregate type, compute the values of the elements by
/// emitting a destructure.
static void getScalarizedElements(SILValue V,
                                  SmallVectorImpl<SILValue> &ElementVals,
                                  SILLocation Loc, SILBuilder &B) {
  auto *DTI = B.createDestructureTuple(Loc, V);
  copy(DTI->getResults(), std::back_inserter(ElementVals));
}

/// Scalarize a load down to its subelements.  If NewLoads is specified, this
/// can return the newly generated sub-element loads.
static SILValue scalarizeLoad(LoadInst *LI,
                              SmallVectorImpl<SILValue> &ElementAddrs) {
  SILBuilderWithScope B(LI);
  SmallVector<SILValue, 4> ElementTmps;

  for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i) {
    auto *SubLI = B.createTrivialLoadOr(LI->getLoc(), ElementAddrs[i],
                                        LI->getOwnershipQualifier());
    ElementTmps.push_back(SubLI);
  }

  if (LI->getType().is<TupleType>())
    return B.createTuple(LI->getLoc(), LI->getType(), ElementTmps);
  return B.createStruct(LI->getLoc(), LI->getType(), ElementTmps);
}

//===----------------------------------------------------------------------===//
//                          Tuple Scalarizer Visitor
//===----------------------------------------------------------------------===//

namespace {

using TupleScalarizerResult = std::pair<bool, SILValue>;

struct TupleScalarizer
  : SILInstructionVisitor<TupleScalarizer, TupleScalarizerResult>> {

  SILFunction &F;

  /// Loop over the passed in function and scalarize any memory operations on
  /// tuples.
  bool process();

  TupleScalarizerResult visitSILInstruction(SILInstruction *) { llvm_unreachable("unimplemented"); }

  /// Attempt to split a load of a tuple into a tuple of loads.
  ///
  ///
  TupleScalarizerResult visitLoadInst(LoadInst *LI);

  TupleScalarizerResult visitStoreInst(StoreInst *SI);

  TupleScalarizerResult visitAssignInst(AssignInst *AI);

  TupleScalarizerResult visitCopyAddrInst(CopyAddrInst *CAI);
};

} // end anonymous namespace

bool TupleScalarizer::process() {
  bool madeChange = false;
  for (auto &BB : F) {
    for (auto II = BB.begin(), IE = BB.end(); II != IE;) {
      auto visitResult = visit(&*II);
      if (auto *LI = dyn_cast<LoadInst>(&II)) {
        if (LI->getOperand()->getType().is<TupleType>()) {
          // First scalarize LI.
          SILValue newResult = scalarizeLoad(LI);
          LI->replaceAllUsesWith(newResult);

          // Then move II past LI (to make sure we recursively scalarize) and
          // delete LI.
          ++II;
          LI->eraseFromParent();
          MadeChange = true;
          continue;
        }
      }

      if (auto *SI = dyn_cast<StoreInst>(&II)) {
        if (SI->getDest()->getType().is<TupleType>()) {          
          scalarizeStore(SI);

          ++II;
          SI->eraseFromParent();
          madeChange = true;
          continue;
        }
      }

      if (auto *AI = dyn_cast<AssignInst>(User)) {
        scalarizeAssign(AI);

        ++II;
        AI->eraseFromParent();
        madeChange = true;
        continue;
      }

      if (auto *CAI = dyn_cast<CopyAddrInst>(&II)) {
        scalarizeCopyAddrInst(CAI);
        ++II;
        CAI->eraseFromParent();
        madeChange = true;
        continue;
      }

      // Otherwise, just look at the next instruction.
      ++II;
    }
  }

  return MadeChange;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class GuaranteedTupleScalarizer : public SILFunctionTransform {
  /// The entry point to the transformation.
  void run() override {
    if (TupleScalarizer(*getFunction()).scalarize())
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createGuaranteedTupleScalarizer() {
  return new GuaranteedTupleScalarizer();
}
