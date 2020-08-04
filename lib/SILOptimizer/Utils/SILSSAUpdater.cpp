//===--- SILSSAUpdater.cpp - Unstructured SSA Update Tool -----------------===//
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

#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"
#include "swift/Basic/Malloc.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/SSAUpdaterImpl.h"

using namespace swift;

void *SILSSAUpdater::allocate(unsigned size, unsigned align) const {
  return AlignedAlloc(size, align);
}

void SILSSAUpdater::deallocateSentinel(SILUndef *undef) {
  AlignedFree(undef);
}

SILSSAUpdater::SILSSAUpdater(SmallVectorImpl<SILPhiArgument *> *phis)
    : blockToAvailableValueMap(nullptr),
      phiSentinel(nullptr, deallocateSentinel), insertedPhis(phis) {}

SILSSAUpdater::~SILSSAUpdater() = default;

void SILSSAUpdater::initialize(SILType inputType) {
  type = inputType;

  phiSentinel = std::unique_ptr<SILUndef, void (*)(SILUndef *)>(
      SILUndef::getSentinelValue(type, this),
      SILSSAUpdater::deallocateSentinel);

  if (!blockToAvailableValueMap)
    blockToAvailableValueMap.reset(new AvailableValsTy());
  else
    blockToAvailableValueMap->clear();
}

bool SILSSAUpdater::hasValueForBlock(SILBasicBlock *block) const {
  return blockToAvailableValueMap->count(block);
}

/// Indicate that a rewritten value is available in the specified block with the
/// specified value.
void SILSSAUpdater::addAvailableValue(SILBasicBlock *block, SILValue value) {
  (*blockToAvailableValueMap)[block] = value;
}

/// Construct SSA form, materializing a value that is live at the end of the
/// specified block.
SILValue SILSSAUpdater::getValueAtEndOfBlock(SILBasicBlock *block) {
  return getValueAtEndOfBlockInternal(block);
}

/// Are all available values identicalTo each other.
static bool areIdentical(
    llvm::DenseMap<SILBasicBlock *, SILValue> &blockToAvailableValueMap) {
  if (auto *firstInst = dyn_cast<SingleValueInstruction>(
          blockToAvailableValueMap.begin()->second)) {
    for (auto pair : blockToAvailableValueMap) {
      auto *inst = dyn_cast<SingleValueInstruction>(pair.second);
      if (!inst)
        return false;
      if (!inst->isIdenticalTo(firstInst))
        return false;
    }
    return true;
  }

  auto *mvir = dyn_cast<MultipleValueInstructionResult>(
      blockToAvailableValueMap.begin()->second);
  if (!mvir)
    return false;

  for (auto pair : blockToAvailableValueMap) {
    auto *Result = dyn_cast<MultipleValueInstructionResult>(pair.second);
    if (!Result)
      return false;
    if (!Result->getParent()->isIdenticalTo(mvir->getParent()) ||
        Result->getIndex() != mvir->getIndex()) {
      return false;
    }
  }
  return true;
}

/// This should be called in top-down order of each def that needs its uses
/// rewrited. The order that we visit uses for a given def is irrelevant.
void SILSSAUpdater::rewriteUse(Operand &op) {
  // Replicate function_refs to their uses. SILGen can't build phi nodes for
  // them and it would not make much sense anyways.
  if (auto *fri = dyn_cast<FunctionRefInst>(op.get())) {
    assert(areIdentical(*blockToAvailableValueMap) &&
           "The function_refs need to have the same value");
    SILInstruction *user = op.getUser();
    auto *newFRI = cast<FunctionRefInst>(fri->clone(user));
    op.set(newFRI);
    return;
  }

  if (auto *pdfri = dyn_cast<PreviousDynamicFunctionRefInst>(op.get())) {
    assert(areIdentical(*blockToAvailableValueMap) &&
           "The function_refs need to have the same value");
    SILInstruction *user = op.getUser();
    auto *newInst = cast<PreviousDynamicFunctionRefInst>(pdfri->clone(user));
    op.set(newInst);
    return;
  }

  if (auto *dfri = dyn_cast<DynamicFunctionRefInst>(op.get())) {
    assert(areIdentical(*blockToAvailableValueMap) &&
           "The function_refs need to have the same value");
    SILInstruction *User = op.getUser();
    auto *newInst = cast<DynamicFunctionRefInst>(dfri->clone(User));
    op.set(newInst);
    return;
  }

  if (auto *ili = dyn_cast<IntegerLiteralInst>(op.get()))
    if (areIdentical(*blockToAvailableValueMap)) {
      // Some llvm intrinsics don't like phi nodes as their constant inputs (e.g
      // ctlz).
      SILInstruction *user = op.getUser();
      auto *newInst = cast<IntegerLiteralInst>(ili->clone(user));
      op.set(newInst);
      return;
    }

  // Again we need to be careful here, because ssa construction (with the
  // existing representation) can change the operand from under us.
  UseWrapper useWrapper(&op);

  SILInstruction *user = op.getUser();
  SILValue newVal = getValueInMiddleOfBlock(user->getParent());
  assert(newVal && "Need a valid value");
  ((Operand *)useWrapper)->set((SILValue)newVal);
}

/// Get the edge values from the terminator to the destination basic block.
static OperandValueArrayRef getEdgeValuesForTerminator(TermInst *ti,
                                                       SILBasicBlock *toBlock) {
  if (auto *br = dyn_cast<BranchInst>(ti)) {
    assert(br->getDestBB() == toBlock &&
           "Incoming edge block and phi block mismatch");
    return br->getArgs();
  }

  if (auto *cbi = dyn_cast<CondBranchInst>(ti)) {
    bool isTrueEdge = cbi->getTrueBB() == toBlock;
    assert(((isTrueEdge && cbi->getTrueBB() == toBlock) ||
            cbi->getFalseBB() == toBlock) &&
           "Incoming edge block and phi block mismatch");
    return isTrueEdge ? cbi->getTrueArgs() : cbi->getFalseArgs();
  }

  // We need a predecessor who is capable of holding outgoing branch
  // arguments.
  llvm_unreachable("Unrecognized terminator leading to phi block");
}

/// Check that the argument has the same incoming edge values as the value
/// map.
static bool
isEquivalentPHI(SILPhiArgument *phi,
                llvm::SmallDenseMap<SILBasicBlock *, SILValue, 8> &valueMap) {
  SILBasicBlock *phiBlock = phi->getParent();
  size_t phiArgIndex = phi->getIndex();
  for (auto *predBlock : phiBlock->getPredecessorBlocks()) {
    auto desiredValue = valueMap[predBlock];
    OperandValueArrayRef edgeValues =
        getEdgeValuesForTerminator(predBlock->getTerminator(), phiBlock);
    if (edgeValues[phiArgIndex] != desiredValue)
      return false;
  }
  return true;
}

SILValue SILSSAUpdater::getValueInMiddleOfBlock(SILBasicBlock *block) {
  // If this basic block does not define a value we can just use the value
  // live at the end of the block.
  if (!hasValueForBlock(block))
    return getValueAtEndOfBlock(block);

  /// Otherwise, we have to build SSA for the value defined in this block and
  /// this block's predecessors.
  SILValue singularValue;
  SmallVector<std::pair<SILBasicBlock *, SILValue>, 4> predVals;
  bool firstPred = true;

  // SSAUpdater can modify TerminatorInst and therefore invalidate the
  // predecessor iterator. Find all the predecessors before the SSA update.
  SmallVector<SILBasicBlock *, 4> predBlocks;
  for (auto *block : block->getPredecessorBlocks()) {
    predBlocks.push_back(block);
  }

  for (auto *block : predBlocks) {
    SILValue value = getValueAtEndOfBlock(block);
    predVals.push_back(std::make_pair(block, value));
    if (firstPred) {
      singularValue = value;
      firstPred = false;
    } else if (singularValue != value)
      singularValue = SILValue();
  }

  // Return undef for blocks without predecessor.
  if (predVals.empty())
    return SILUndef::get(type, *block->getParent());

  if (singularValue)
    return singularValue;

  // Check if we already have an equivalent phi.
  if (!block->getArguments().empty()) {
    llvm::SmallDenseMap<SILBasicBlock *, SILValue, 8> valueMap(predVals.begin(),
                                                               predVals.end());
    for (auto *arg : block->getSILPhiArguments())
      if (isEquivalentPHI(arg, valueMap))
        return arg;
  }

  // Create a new phi node.
  SILPhiArgument *phi =
      block->createPhiArgument(type, ValueOwnershipKind::Owned);

  for (auto &pair : predVals)
    addNewEdgeValueToBranch(pair.first->getTerminator(), block, pair.second);

  if (insertedPhis)
    insertedPhis->push_back(phi);

  return phi;
}

/// SSAUpdaterTraits<MachineSSAUpdater> - Traits for the SSAUpdaterImpl
/// template, specialized for MachineSSAUpdater.
namespace llvm {
template<>
class SSAUpdaterTraits<SILSSAUpdater> {
public:
  typedef SILBasicBlock BlkT;
  typedef SILValue ValT;
  typedef SILPhiArgument PhiT;

  typedef SILBasicBlock::succ_iterator BlkSucc_iterator;
  static BlkSucc_iterator BlkSucc_begin(BlkT *block) {
    return block->succ_begin();
  }
  static BlkSucc_iterator BlkSucc_end(BlkT *block) { return block->succ_end(); }

  /// Iterator for PHI operands.
  class PHI_iterator {
  private:
    SILBasicBlock::pred_iterator predecessorIterator;
    SILBasicBlock *block;
    size_t argumentIndex;

  public:
    explicit PHI_iterator(SILPhiArgument *P) // begin iterator
        : predecessorIterator(P->getParent()->pred_begin()),
          block(P->getParent()), argumentIndex(P->getIndex()) {}
    PHI_iterator(SILPhiArgument *P, bool) // end iterator
        : predecessorIterator(P->getParent()->pred_end()),
          block(P->getParent()), argumentIndex(P->getIndex()) {}

    PHI_iterator &operator++() {
      ++predecessorIterator;
      return *this;
    }
    bool operator==(const PHI_iterator &x) const {
      return predecessorIterator == x.predecessorIterator;
    }
    bool operator!=(const PHI_iterator& x) const { return !operator==(x); }

    SILValue getValueForBlock(size_t argumentIndex, SILBasicBlock *block,
                              TermInst *ti) {
      OperandValueArrayRef args = getEdgeValuesForTerminator(ti, block);
      assert(argumentIndex < args.size() &&
             "Not enough values on incoming edge");
      return args[argumentIndex];
    }

    SILValue getIncomingValue() {
      return getValueForBlock(argumentIndex, block,
                              (*predecessorIterator)->getTerminator());
    }

    SILBasicBlock *getIncomingBlock() { return *predecessorIterator; }
  };

  static inline PHI_iterator PHI_begin(PhiT *phi) { return PHI_iterator(phi); }
  static inline PHI_iterator PHI_end(PhiT *phi) {
    return PHI_iterator(phi, true);
  }

  /// Put the predecessors of BB into the Preds vector.
  static void
  FindPredecessorBlocks(SILBasicBlock *block,
                        SmallVectorImpl<SILBasicBlock *> *predBlocks) {
    assert(predBlocks);
    llvm::copy(block->getPredecessorBlocks(), std::back_inserter(*predBlocks));
  }

  static SILValue GetUndefVal(SILBasicBlock *block, SILSSAUpdater *updater) {
    return SILUndef::get(updater->type, *block->getParent());
  }

  /// Add an Argument to the basic block.
  static SILValue CreateEmptyPHI(SILBasicBlock *block, unsigned numPreds,
                                 SILSSAUpdater *updater) {
    // Add the argument to the block.
    SILValue phi(
        block->createPhiArgument(updater->type, ValueOwnershipKind::Owned));

    // Mark all predecessor blocks with the sentinel undef value.
    SmallVector<SILBasicBlock *, 4> predBlocks(block->getPredecessorBlocks());
    for (auto *block : predBlocks) {
      TermInst *ti = block->getTerminator();
      addNewEdgeValueToBranch(ti, block, updater->phiSentinel.get());
    }
    return phi;
  }

  /// Add the specified value as an operand of the PHI for the specified
  /// predecessor block.
  static void AddPHIOperand(SILPhiArgument *phi, SILValue value,
                            SILBasicBlock *predBlock) {
    auto *phiBlock = phi->getParent();
    size_t phiArgIndex = phi->getIndex();
    auto *ti = predBlock->getTerminator();
    changeEdgeValue(ti, phiBlock, phiArgIndex, value);
  }

  /// InstrIsPHI - Check if an instruction is a PHI.
  ///
  static SILPhiArgument *InstrIsPHI(ValueBase *value) {
    return dyn_cast<SILPhiArgument>(value);
  }

  /// ValueIsPHI - Check if the instruction that defines the specified register
  /// is a PHI instruction.
  static SILPhiArgument *ValueIsPHI(SILValue value, SILSSAUpdater *updater) {
    return InstrIsPHI(value);
  }

  /// Like ValueIsPHI but also check if the PHI has no source
  /// operands, i.e., it was just added.
  static SILPhiArgument *ValueIsNewPHI(SILValue value, SILSSAUpdater *updater) {
    SILPhiArgument *phi = ValueIsPHI(value, updater);
    if (phi) {
      auto *phiBlock = phi->getParent();
      size_t phiArgIndex = phi->getIndex();

      // If all predecessor edges are 'not set' this is a new phi.
      for (auto *predBlock : phiBlock->getPredecessorBlocks()) {
        OperandValueArrayRef edges =
            getEdgeValuesForTerminator(predBlock->getTerminator(), phiBlock);

        assert(phiArgIndex < edges.size() && "Not enough edges!");

        SILValue edgeValue = edges[phiArgIndex];
        // Check for the 'not set' sentinel.
        if (edgeValue != updater->phiSentinel.get())
          return nullptr;
      }
      return phi;
    }
    return nullptr;
  }

  static SILValue GetPHIValue(SILPhiArgument *phi) { return phi; }
};

} // namespace llvm

/// Check to see if AvailableVals has an entry for the specified BB and if so,
/// return it.  If not, construct SSA form by first calculating the required
/// placement of PHIs and then inserting new PHIs where needed.
SILValue SILSSAUpdater::getValueAtEndOfBlockInternal(SILBasicBlock *block) {
  AvailableValsTy &availableVals = *blockToAvailableValueMap;
  auto iter = availableVals.find(block);
  if (iter != availableVals.end())
    return iter->second;

  llvm::SSAUpdaterImpl<SILSSAUpdater> Impl(this, &availableVals, insertedPhis);
  return Impl.GetValue(block);
}

/// Construct a use wrapper. For branches we store information so that we
/// can reconstruct the use after the branch has been modified.
///
/// When a branch is modified existing pointers to the operand
/// (ValueUseIterator) become invalid as they point to freed operands.  Instead
/// we store the branch's parent and the idx so that we can reconstruct the use.
UseWrapper::UseWrapper(Operand *inputUse) {
  wrappedUse = nullptr;
  type = kRegularUse;

  SILInstruction *inputUser = inputUse->getUser();

  // Direct branch user.
  if (auto *br = dyn_cast<BranchInst>(inputUser)) {
    auto operands = inputUser->getAllOperands();
    for (unsigned i = 0, e = operands.size(); i != e; ++i) {
      if (inputUse == &operands[i]) {
        index = i;
        type = kBranchUse;
        parent = br->getParent();
        return;
      }
    }
  }

  // Conditional branch user.
  if (auto *cbi = dyn_cast<CondBranchInst>(inputUser)) {
    auto operands = inputUser->getAllOperands();
    unsigned numTrueArgs = cbi->getTrueArgs().size();
    for (unsigned i = 0, e = operands.size(); i != e; ++i) {
      if (inputUse == &operands[i]) {
        // We treat the condition as part of the true args.
        if (i < numTrueArgs + 1) {
          index = i;
          type = kCondBranchUseTrue;
        } else {
          index = i - numTrueArgs - 1;
          type = kCondBranchUseFalse;
        }
        parent = cbi->getParent();
        return;
      }
    }
  }

  wrappedUse = inputUse;
}

/// Return the operand we wrap. Reconstructing branch operands.
Operand *UseWrapper::getOperand() {
  switch (type) {
  case kRegularUse:
    return wrappedUse;

  case kBranchUse: {
    auto *br = cast<BranchInst>(parent->getTerminator());
    assert(index < br->getNumArgs());
    return &br->getAllOperands()[index];
  }

  case kCondBranchUseTrue:
  case kCondBranchUseFalse: {
    auto *cbi = cast<CondBranchInst>(parent->getTerminator());
    unsigned IdxToUse =
        type == kCondBranchUseTrue ? index : cbi->getTrueArgs().size() + 1 + index;
    assert(IdxToUse < cbi->getAllOperands().size());
    return &cbi->getAllOperands()[IdxToUse];
  }
  }

  llvm_unreachable("uninitialize use type");
}

/// At least one value feeding the specified SILArgument is a Struct. Attempt to
/// replace the Argument with a new Struct in the same block.
///
/// When we handle more types of casts, this can become a template.
///
/// ArgValues are the values feeding the specified Argument from each
/// predecessor. They must be listed in order of Arg->getParent()->getPreds().
static StructInst *
replaceBBArgWithStruct(SILPhiArgument *phi,
                       SmallVectorImpl<SILValue> &argValues) {
  SILBasicBlock *phiBlock = phi->getParent();
  auto *firstSI = dyn_cast<StructInst>(argValues[0]);
  if (!firstSI)
    return nullptr;

  // Collect the BBArg index of each struct oper.
  // e.g.
  //   struct(A, B)
  //   br (B, A)
  // : ArgIdxForOper => {1, 0}
  SmallVector<unsigned, 4> argIdxForOper;
  for (unsigned operIdx : indices(firstSI->getElements())) {
    bool foundMatchingArgIdx = false;
    for (unsigned argIdx : indices(phiBlock->getArguments())) {
      auto availableValueIter = argValues.begin();
      bool tryNextArgIndex = false;
      for (auto *predblock : phiBlock->getPredecessorBlocks()) {
        // all argument values must be structinst.
        auto *predsi = dyn_cast<StructInst>(*availableValueIter++);
        if (!predsi)
          return nullptr;
        auto edgeValues =
            getEdgeValuesForTerminator(predblock->getTerminator(), phiBlock);
        if (edgeValues[argIdx] != predsi->getElements()[operIdx]) {
          tryNextArgIndex = true;
          break;
        }
      }
      if (!tryNextArgIndex) {
        assert(availableValueIter == argValues.end() &&
               "# argvalues does not match # bb preds");
        foundMatchingArgIdx = true;
        argIdxForOper.push_back(argIdx);
        break;
      }
    }
    if (!foundMatchingArgIdx)
      return nullptr;
  }

  SmallVector<SILValue, 4> structArgs;
  for (auto argidx : argIdxForOper)
    structArgs.push_back(phiBlock->getArgument(argidx));

  // todo: what is the right debug scope to use here!
  SILBuilder builder(phiBlock, phiBlock->begin());
  return builder.createStruct(cast<StructInst>(argValues[0])->getLoc(),
                              phi->getType(), structArgs);
}

/// canonicalize bb arguments, replacing argument-of-casts with
/// cast-of-arguments. this only eliminates existing arguments, replacing them
/// with casts. no new arguments are created. this allows downstream pattern
/// detection like induction variable analysis to succeed.
///
/// if arg is replaced, return the cast instruction. Otherwise return nullptr.
SILValue swift::replaceBBArgWithCast(SILPhiArgument *arg) {
  SmallVector<SILValue, 4> argValues;
  arg->getIncomingPhiValues(argValues);
  if (isa<StructInst>(argValues[0]))
    return replaceBBArgWithStruct(arg, argValues);
  return nullptr;
}
