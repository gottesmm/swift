//===---- TupleMemoryOperationCanonicalizer.cpp ---------------------------===//
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
/// instead, we use this pass to expand those tuple typed loads, stores,
/// assigns, copy_addr and runs before both of those optimizations to ensure
/// that we can maximize our analysis/optimization opportunities.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "tuple-memop-canoncializer"

using namespace swift;

//===----------------------------------------------------------------------===//
//                          Scalarization Logic
//===----------------------------------------------------------------------===//

/// Given a pointer to a tuple type, compute the addresses of each element and
/// add them to the ElementAddrs vector.
static void
getScalarizedElementAddresses(SILValue Pointer, SILBuilder &B, SILLocation Loc,
                              SmallVectorImpl<SILValue> &ElementAddrs) {
v  TupleType *TT = Pointer->getType().castTo<TupleType>();
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

  /// Attempt to split a load of a tuple into a tuple of loads. Returns {true,
  /// newLoad} on success. {false, SILValue()} otherwise.
  ///
  /// *NOTE* This does not delete the original instruction. This is left to the
  /// caller so that the caller can avoid iterator invalidation if they choose
  /// to.
  TupleScalarizerResult visitLoadInst(LoadInst *LI);

  /// Attempt to split a store of a tuple into a set of stores for each tuple
  /// element. Returns {true, SILValue()} on success. {false, SILValue()}
  /// otherwise.
  ///
  /// *NOTE* This does not delete the original instruction. This is left to the
  /// caller so that the caller can avoid iterator invalidation if they choose
  /// to.
  TupleScalarizerResult visitStoreInst(StoreInst *SI);

  /// Attempt to split an assign of a tuple into a set of assigns for each tuple
  /// element. Returns {true, SILValue()} on success. {false, SILValue()}
  /// otherwise.
  ///
  /// *NOTE* This does not delete the original instruction. This is left to the
  /// caller so that the caller can avoid iterator invalidation if they choose
  /// to.
  TupleScalarizerResult visitAssignInst(AssignInst *AI);

  /// Attempt to split a copy_addr of a tuple into a set of copy_addr for each
  /// tuple element. Returns {true, SILValue()} on success. {false, SILValue()}
  /// otherwise.
  ///
  /// *NOTE* This does not delete the original instruction. This is left to the
  /// caller so that the caller can avoid iterator invalidation if they choose
  /// to.
  TupleScalarizerResult visitCopyAddrInst(CopyAddrInst *CAI);
};

} // end anonymous namespace

TupleScalarizerResult TupleScalarizer::visitLoadInst(LoadInst *LI) {
  if (!LI->getOperand()->getType().is<TupleType>()) {
    return {false, SILValue()};
  }

  SILBuilderWithScope B(LI);
  SmallVector<SILValue, 4> ElementTmps;

  for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i) {
    auto *subLI = B.createTrivialLoadOr(LI->getLoc(), ElementAddrs[i],
                                        LI->getOwnershipQualifier());
    elementTmps.push_back(SubLI);
  }

  SingleValueInstruction *result;
  if (LI->getType().is<TupleType>())
    result = B.createTuple(LI->getLoc(), LI->getType(), ElementTmps);
  result = B.createStruct(LI->getLoc(), LI->getType(), ElementTmps);
}

TupleScalarizerResult TupleScalarizer::visitStoreInst(StoreInst *SI) {
  if (!SI->getDest()->getType().is<TupleType>()) {
    return {false, SILValue()};
  }
}

TupleScalarizerResult TupleScalarizer::visitAssignInst(AssignInst *AI) {
  if (!AI->getDest()->getType().is<TupleType>()) {
    return {false, SILValue()};
  }
}

TupleScalarizerResult TupleScalarizer::visitCopyAddrInst(CopyAddrInst *CAI) {
  if (!CAI->getDest()->getType().is<TupleType>()) {
    return {false, SILValue()};
  }
}

bool TupleScalarizer::process() {
  bool madeChange = false;

  for (auto &BB : F) {
    assert(BB.getTerminator() && "Expected all blocks to have terminators");
    auto termIter = BB.getTerminator()->getIterator();
    for (auto II = BB.begin(), IE = termIter->getIterator(); II != IE;) {
      // When we expand, SILBuilder is going to insert the expanded instructions
      // /before/ the instruction. So to be sure that we process those
      // instructions:
      //
      // 1. If II is not the first instruction in the block, we move to the
      //    previous instruction and then move forward to the newly expanded
      //    instructions after deleting the load.
      //
      // 2. If II /is/ the first instruction in the block, we move the iterator
      //    to the terminator instead and then move the iterator to the first
      //    instruction after we delete the expanded instruction.
      auto prevIter = prev_or_default(II, BB.begin(), termIter);
      auto *inst = &*II;
      auto result = visit(inst);

      // If we failed to create new instructions, we will not expand the
      // instruction. Move ahead one.
      if (!result.first()) {
        ++II;
        continue;
      }

      // Otherwise, we did succeed in splitting up the given instruction. Erase
      // the instruction and then move the insert point into its new location.
      inst->eraseFromParent();
      II = next_or_default(II, termIter, BB.begin());
      madeChange = true;
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
