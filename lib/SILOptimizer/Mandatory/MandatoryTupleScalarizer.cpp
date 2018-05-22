//===--- MandatoryTupleScalarizer.cpp -------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-mandatory-tuple-scalarizer"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                          Scalarization Logic
//===----------------------------------------------------------------------===//

/// Given a pointer to a tuple type, compute the addresses of each element and
/// add them to the ElementAddrs vector.
static void
getScalarizedElementAddresses(SILValue pointer, SILBuilder &b, SILLocation loc,
                              SmallVectorImpl<SILValue> &elementAddrs) {
  auto tt = pointer->getType().castTo<TupleType>();
  for (unsigned index : indices(tt->getElements())) {
    elementAddrs.push_back(b.createTupleElementAddr(loc, pointer, index));
  }
}

namespace {

struct TupleScalarizer : SILInstructionVisitor<TupleScalarizer, bool> {
  /// A worklist we use to break down tuples recursively.
  SmallVector<SILInstruction *, 32> worklist;

  ~TupleScalarizer() {
    assert(worklist.empty() &&
           "tuple scalarizer destroyed without worklist being emptied?!");
  }

  /// Default implementation. Returns false.
  bool visitSILInstruction(SILInstruction *) { return false; }

  bool visitLoadInst(LoadInst *li);
  bool visitStoreInst(StoreInst *si);
  bool visitAssignInst(AssignInst *ai);
  bool visitCopyAddrInst(CopyAddrInst *cai);

  bool process(SILFunction &fn);
};

} // end anonymous namespace

bool TupleScalarizer::visitLoadInst(LoadInst *li) {
  SILType objType = li->getType();

  // If we are not loading from a tuple, bail.
  if (!objType.is<TupleType>())
    return false;

  // Otherwise split up the tuple and then add the new tuple elements to the
  // worklist for additional processing.
  SILBuilderWithScope b(li);
  SILLocation loc = li->getLoc();

  SmallVector<SILValue, 32> elementTmps;
  getScalarizedElementAddresses(li->getOperand(), b, loc, elementTmps);

  for (unsigned i = 0, e = elementTmps.size(); i != e; ++i) {
    auto *newLoad =
        b.createTrivialLoadOr(loc, elementTmps[i], li->getOwnershipQualifier());
    if (newLoad->getType().is<TupleType>())
      worklist.push_back(newLoad);
  }

  auto *tup = b.createTuple(li->getLoc(), li->getType(), elementTmps);
  li->replaceAllUsesWith(tup);
  li->eraseFromParent();
  return true;
}

bool TupleScalarizer::visitStoreInst(StoreInst *si) {
  SILType objType = si->getSrc()->getType();
  if (!objType.is<TupleType>())
    return false;

  SILBuilderWithScope b(si);
  SILLocation loc = si->getLoc();

  SmallVector<SILValue, 32> elementAddrs;
  getScalarizedElementAddresses(si->getDest(), b, loc, elementAddrs);

  b.emitDestructureValueOperation(
      loc, si->getSrc(), [&](unsigned index, SILValue elt) {
        auto *newSI = b.createTrivialStoreOr(loc, elt, elementAddrs[index],
                                             si->getOwnershipQualifier());
        if (newSI->getSrc()->getType().is<TupleType>())
          worklist.push_back(newSI);
      });
  si->eraseFromParent();
  return true;
}

bool TupleScalarizer::visitAssignInst(AssignInst *ai) {
  SILType objType = ai->getSrc()->getType();
  if (!objType.is<TupleType>())
    return false;

  SILBuilderWithScope b(ai);
  SILLocation loc = ai->getLoc();

  SmallVector<SILValue, 32> elementAddrs;
  getScalarizedElementAddresses(ai->getDest(), b, loc, elementAddrs);

  b.emitDestructureValueOperation(
      loc, ai->getSrc(), [&](unsigned index, SILValue elt) {
        auto *newAI = b.createAssign(loc, elt, elementAddrs[index]);
        if (elt->getType().is<TupleType>())
          worklist.push_back(newAI);
      });

  ai->eraseFromParent();
  return true;
}

bool TupleScalarizer::visitCopyAddrInst(CopyAddrInst *cai) {
  SILType type = cai->getSrc()->getType();
  if (!type.is<TupleType>())
    return false;

  SILBuilderWithScope b(cai);
  SILLocation loc = cai->getLoc();

  SmallVector<SILValue, 32> srcAddrs;
  getScalarizedElementAddresses(cai->getSrc(), b, loc, srcAddrs);
  SmallVector<SILValue, 32> destAddrs;
  getScalarizedElementAddresses(cai->getDest(), b, loc, destAddrs);

  for (auto p : llvm::zip(srcAddrs, destAddrs)) {
    SILValue src, dest;
    std::tie(src, dest) = p;
    auto *newCAI = b.createCopyAddr(loc, src, dest, cai->isTakeOfSrc(),
                                    cai->isInitializationOfDest());
    if (newCAI->getSrc()->getType().is<TupleType>())
      worklist.push_back(newCAI);
  }

  cai->eraseFromParent();
  return true;
}

bool TupleScalarizer::process(SILFunction &fn) {
  bool madeChange = false;

  for (auto &bb : fn) {
    // First visit all of the instructions in the block. Our routines put any
    // tuple values that must be recursively processed into the worklist.
    for (auto ii = bb.begin(), ie = bb.end(); ii != ie;) {
      auto *inst = &*ii;
      ++ii;
      madeChange |= visit(inst);
    }

    // Then recursively process the worklist, performing any recursive tuple
    // destructuring we may need.
    while (!worklist.empty()) {
      visit(worklist.pop_back_val());
    }
  }

  return madeChange;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class MandatoryTupleScalarizer : public SILFunctionTransform {
  void run() override {
    // Even though this is technically not a diagnostic pass, it is part of
    // preparing the module for DI, so once we have run DI there is no point in
    // running this again.
    if (getFunction()->wasDeserializedCanonical())
      return;

    if (TupleScalarizer().process(*getFunction())) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createMandatoryTupleScalarizer() {
  return new MandatoryTupleScalarizer();
}
