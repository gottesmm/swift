//===--- DebugInfoCanonicalizer.cpp ---------------------------------------===//
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
///
/// This file contains transformations that propagate debug info at the SIL
/// level to make IRGen's job easier. The specific transformations that we
/// perform is that we clone dominating debug_value for a specific
/// SILDebugVariable after all coroutine-func-let boundary instructions. This in
/// practice this as an algorithm works as follows:
///
/// 1. We go through the function and gather up "interesting instructions" that
///    are funclet boundaries.
///
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-onone-debuginfo-canonicalizer"

#include "swift/Basic/LLVM.h"
#include "swift/Basic/BlotMapVector.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/SILValue.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallSet.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

static void cloneDebugValue(DebugValueInst *original,
                            SILInstruction *insertPt) {
  if (auto *ti = dyn_cast<TermInst>(insertPt)) {
    for (auto *succBlock : ti->getSuccessorBlocks()) {
      auto *result = original->clone(&*succBlock->begin());
      cast<DebugValueInst>(result)->markTransformInserted();
    }
    return;
  }

  auto *result = original->clone(&*std::next(insertPt->getIterator()));
  cast<DebugValueInst>(result)->markTransformInserted();
}

//===----------------------------------------------------------------------===//
//                               Implementation
//===----------------------------------------------------------------------===//

namespace {

struct BlockState {
  // A map from a SILDebugVariable to the last debug_value associated with it in
  // this block.
  SmallBlotMapVector<SILDebugVariable, DebugValueInst *, 4> firstDebugValues;
  SmallBlotMapVector<SILDebugVariable, DebugValueInst *, 4> lastDebugValues;
};

struct DebugInfoCanonicalizer {
  SILFunction *fn;
  DominanceAnalysis *da;
  DominanceInfo *dt;
  PostOrderAnalysis *poa;
  PostOrderFunctionInfo *poi;

  llvm::MapVector<SILBasicBlock *, BlockState> blockToBlockState;

  DebugInfoCanonicalizer(SILFunction *fn, DominanceAnalysis *da,
                         PostOrderAnalysis *poa)
      : fn(fn), da(da), dt(nullptr), poa(poa), poi(nullptr) {}

  // We only need the dominance info if we actually see a funclet boundary. So
  // make this lazy so we only create the dom tree in functions that actually
  // use coroutines.
  DominanceInfo *getDominance() {
    if (!dt)
      dt = da->get(fn);
    return dt;
  }

  PostOrderFunctionInfo *getPostOrderInfo() {
    if (!poi)
      poi = poa->get(fn);
    return poi;
  }

  bool process();
  bool process2();

  /// NOTE: insertPt->getParent() may not equal startBlock! This is b/c if we
  /// are propagating from a yield, we want to begin in the yields block, not
  /// the yield's insertion point successor block.
  bool propagateDebugValuesFromDominators(
      SILInstruction *insertPt, SILBasicBlock *startBlock,
      llvm::SmallDenseSet<SILDebugVariable, 8> &seenDebugVars) {
    LLVM_DEBUG(llvm::dbgs() << "==> PROPAGATING VALUE\n");
    LLVM_DEBUG(llvm::dbgs() << "Inst: " << *insertPt);

    auto *dt = getDominance();
    auto *domTreeNode = dt->getNode(startBlock);
    auto *rootNode = dt->getRootNode();
    if (domTreeNode == rootNode) {
      LLVM_DEBUG(llvm::dbgs() << "Root node! Nothing to propagate!\n");
      return false;
    }

    LLVM_DEBUG(llvm::dbgs()
               << "Root Node: " << rootNode->getBlock()->getDebugID() << '\n');

    // We already emitted in our caller all debug_value needed from the block we
    // were processing. We just need to walk up the dominator tree until we
    // process the root node.
    bool madeChange = false;
    do {
      domTreeNode = domTreeNode->getIDom();
      LLVM_DEBUG(llvm::dbgs() << "Visiting idom: "
                              << domTreeNode->getBlock()->getDebugID() << '\n');
      auto &domBlockState = blockToBlockState[domTreeNode->getBlock()];
      for (auto &pred : domBlockState.lastDebugValues) {
        // If we see a nullptr, we had a SILUndef. Do not clone, but mark this
        // as a debug var we have seen so if it is again defined in previous
        // blocks, we don't clone.
        if (!pred.second) {
          seenDebugVars.insert(pred.first);
          continue;
        }

        LLVM_DEBUG(llvm::dbgs() << "Has DebugValue: " << *pred.second);

        // If we have already inserted something for this debug_value,
        // continue.
        if (!seenDebugVars.insert(pred.first).second) {
          LLVM_DEBUG(llvm::dbgs() << "Already seen this one... skipping!\n");
          continue;
        }

        // Otherwise do the clone.
        LLVM_DEBUG(llvm::dbgs() << "Haven't seen this one... cloning!\n");
        cloneDebugValue(pred.second, insertPt);

        madeChange = true;
      }
    } while (domTreeNode != rootNode);

    return madeChange;
  }
};

} // namespace

static bool isInterestingInst(SILInstruction *inst) {
  // This handles begin_apply.
  if (auto fas = FullApplySite::isa(inst)) {
    if (fas.beginsCoroutineEvaluation())
      return true;
  }
  if (isa<HopToExecutorInst>(inst))
    return true;
  if (isa<EndApplyInst>(inst) || isa<AbortApplyInst>(inst))
    return true;
  return false;
}

struct BlockState2 {
  SmallBitVector entry;
  SmallBitVector gen;
  SmallBitVector kill;
  SmallBitVector exit;
  bool hasInitializedExit = false;

  SILBasicBlock *block;

  BlockState2(SILBasicBlock *block) : block(block) {}
};

bool DebugInfoCanonicalizer::process2() {
  SmallSetVector<SILBasicBlock *, 32> funcLetBoundaryBlocks;
  SmallVector<DebugValueInst *, 32> debugInsts;
  for (auto &block : *fn) {
    for (auto &inst : block) {
      if (isInterestingInst(&inst)) {
        funcLetBoundaryBlocks.insert(&block);
        continue;
      }

      if (auto *dvi = dyn_cast<DebugValueInst>(&inst)) {
        if (dvi->getVarInfo()) {
          debugInsts.push_back(dvi);
          continue;
        }
      }
    }
  }

  // If we don't have any funclet boundaries, just exit. We do not have any work
  // to do.
  if (funcLetBoundaryBlocks.empty())
    return false;

  // Ok, we need to perform dataflow. Initialize our blocks.
  //
  // We could have performed this earlier, but we only want to allocate memory
  // if we are actually going to process.
  using BlockIndexPair = std::pair<SILBasicBlock *, unsigned>;
  std::vector<BlockState2> blockStates;
  llvm::DenseMap<SILBasicBlock *, BlockState2 *> blockToStateMap;
  unsigned numDebugInsts = debugInsts.size();

  // Setup our blocks in rpo order.
  for (auto &pair : llvm::enumerate(getPostOrderInfo()->getReversePostOrder())) {
    BlockState2 &state = blockStates[pair.index()];
    state.block = pair.value();
    state.entry.resize(numDebugInsts);
    state.exit.resize(numDebugInsts);
    blockToStateMap[pair.value()] = &state;
  }

  {
    // In case, we need to malloc, we reuse tmp in between rounds.
    SmallBitVector tmp(numDebugInsts);
    bool firstRound = true;
    bool dataflowChanged = false;
    do {
      dataflowChanged = false;

      for (auto &state : blockStates) {
        tmp = state.entry;

        // Union predecessors.
        for (auto *predBlock : state.block->getPredecessorBlocks()) {
          tmp &= blockToStateMap[predBlock]->exit;
        }

        // If we are in the first round or we made a change in our
        // entry... perform the transfer.
        if (tmp != state.entry || firstRound) {
          state.entry = tmp;
          tmp.reset(state.kill);
          tmp |= state.gen;
          state.exit = tmp;

          // Since we changed, we need to run again.
          dataflowChanged = true;
        }
      }

      firstRound = false;
    } while (dataflowChanged);
  }

  // Ok, we have completed our dataflow, lets clone debug_values. We walk each
  // of our funclet boundary blocks from top to bottom cloning as we go.
  llvm::SmallDenseMap<SILDebugVariable, DebugValueInst *> topState;
  for (auto *block : funcLetBoundaryBlocks) {
    SWIFT_DEFER { topState.clear(); };

    auto &state = blockToStateMap[block];
    for (int setBitIndex = state->entry.find_first(); setBitIndex != -1;
         setBitIndex = state->entry.find_next(setBitIndex)) {
      auto *dvi = debugInsts[setBitIndex];
      topState[*dvi->getVarInfo()] = dvi;
    }

    for (auto &inst : *block) {
      if (isInterestingInst(&inst)) {
        for (auto &pair : topState) {
          cloneDebugValue(pair.second, &inst);
        }
        continue;
      }

      if (auto *dvi = dyn_cast<DebugValueInst>(&inst)) {
        topState[*dvi->getVarInfo()] = dvi;
        continue;
      }
    }
  }
}














































































bool DebugInfoCanonicalizer::process() {
#if false
  bool madeChange = false;

  FrozenMultiMap<SILDebugVariable, DebugValueInst *> debugVarToDebugValues;
  FrozenMultiMap<SILBasicBlock *, SILInstruction *> blockToInterestingInsts;

  {
    BasicBlockWorklist worklist(&*fn->begin());

    while (auto *block = worklist.pop()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "BB: Visiting. bb" << block->getDebugID() << '\n');

      // Then for each instruction in the block...
      auto &blockState = blockToBlockState[block];
      for (auto &inst : *block) {
        // If we have a debug_value with varInfo...
        if (auto *dvi = dyn_cast<DebugValueInst>(&inst)) {
          if (auto varInfo = dvi->getVarInfo()) {
            // Always unconditionally map varInfo to this DVI. We want to track
            // in each block the last debug_value associated with a specific
            // SILDebugVariable.
            auto iter = blockState.lastDebugValues.insert({*varInfo, dvi});

            // If we did not insert, then update iter.first.
            if (!iter.second) {
              (*iter.first)->second = dvi;
            } else {
              // If we did insert, then this was the first debug_value in this
              // block. Track this.
              blockState.firstDebugValues.insert({*varInfo, dvi});
            }
            continue;
          }
        }

        // If we find an interesting inst, dump the current set of inblock
        // debug_var that we are tracking. Also add it to the
        // blockToInterestingInsts multi-map for use in the global dataflow
        // case.
        if (isInterestingInst(&inst)) {
          for (auto &pair : blockState.lastDebugValues) {
            cloneDebugValue(pair->second, &inst);
          }
          blockToInterestingInsts.insert(block, &inst);
          continue;
        }
      }

      // Once we have finished processing this blocks instructions, walk through
      // blockState.debugValues and put all last debug_values into the global
      // debug_value that we are processing.
      for (auto &pair : blockState.lastDebugValues) {
        debugVarToDebugValues.insert(pair->first, pair->second);
      }
    }
  }

  // Sort our multi-maps so that they are now in multi-map form.
  debugVarToDebugValues.setFrozen();
  blockToInterestingInsts.setFrozen();

  // Ok at this point, we are prepared to perform our global dataflow. The
  // information that we have gathered are:
  //
  // 1. We know that any coroutine edges that were in the same block as any of
  //    our debug_value were handled earlier when we were gathering things. Thus
  //    we only need to handle the last debug_value in each block which we
  //    tracked above and only coroutine edges that are not in its own block.
  //
  // 2. We also tracked above the first debug_value for each SILDebugVariable in
  //    each block. This acts as a kill and allows us to properly handle
  //    coroutine edges that happen in a block before any other debug_value have
  //    been seen.
  SmallSetVector<DebugValueInst *, 8> undefDebugValue;
  for (auto pair : debugVarToDebugValues.getRange()) {
    SWIFT_DEFER { undefDebugValue.clear(); };

    auto ii = pair.second.begin();
    DebugValueInst *dominatingDebugValue = *ii;

    // Can this ever happen early? I don't think so.
    assert(!isa<SILUndef>(dominatingDebugValue->getOperand()));

    auto *dominatingBlock = dominatingDebugValue->getParent();
    ++ii;
    auto restRange = llvm::make_range(ii, pair.second.end());

    // If we only found a single debug_value for this SILDebugValue, then just
    // visit successors placing debug_value at all interesting instructions that
    // we reach.
    if (ii == pair.second.end()) {
      BasicBlockWorklist worklist(dominatingBlock);
      while (auto *block = worklist.pop()) {
        if (auto range = blockToInterestingInsts.find(block)) {
          for (auto *inst : *range) {
            cloneDebugValue(dominatingDebugValue, inst);
          }
        }

        for (auto *succBlock : block->getSuccessorBlocks())
          worklist.pushIfNotVisited(succBlock);
      }
      continue;
    }

    // Ok, lets first create a BasicBlock worklist, already visit our dominating
    // block and then add our dominating block's successor blocks.
    BasicBlockWorklist worklist(dominatingBlock);
    worklist.pop();
    for (auto *succBlocks : dominatingBlock->getSuccessorBlocks()) {
      worklist.pushIfNotVisited(succBlocks);
    }

    // Now we visit blocks until we run out of blocks.
    while (auto *block = worklist.pop()) {
      auto &state = blockToBlockState[block];

      for (auto *succBlock : block->getSuccessorBlocks())
        worklist.pushIfNotVisited(succBlock);
    }
  }

    // First handle the case where our dominating debug value has other
    // debug_value in the same block. In that case, walk from
    // dominatingDebugValue to end of block, cloning debug values if we see any
    // interesting insts. Once we have reached the end, we assign the last seen
    // debug_value to dominating debug_value and then proceed below.
    if (llvm::any_of(restRange, [&](DebugValueInst *dvi) {
      return dvi->getParent() == dominatingBlock;
    })) {
      // We update currDebugValue in the loop below so that when we are done it
      // contains the last debug_value in the given block.
      auto *currDebugValue = dominatingDebugValue;
      for (auto ii = std::next(dominatingDebugValue->getIterator()),
             ie = dominatingBlock->end(); ii != ie; ++ii) {
        if (auto *dvi = dyn_cast<DebugValueInst>(&*ii)) {
          if (auto varInfo = dvi->getVarInfo()) {
            if (*varInfo == *currDebugValue->getVarInfo())
              currDebugValue = dvi;
          }
          continue;
        }
        if (isInterestingInst(&*ii)) {
          cloneDebugValue(currDebugValue, &*ii);
        }
      }

      // Reassign dominating debug value to be the last one in the dominating
      // block. This doesn't effect our analysis and allows us to work in a more
      // general way below.
      dominatingDebugValue = currDebugValue;
    }

    // Ok! At this point we have found a single dominating debug_value that is
    // live out of our initial dominating block.
    BasicBlockWorklist innerWorklist(dominatingDebugValue->getParent());
    while (auto *block = innerWorklist.pop()) {
      if (auto range = blockToInterestingInsts.find(block)) {
        for (auto *insts : *range) {
          
        }
      }

      for (auto *succBlock : block->getSuccessorBlocks())
        innerWorklist.pushIfNotVisited(succBlock);
    }
  }

  // We perform an optimistic dataflow with the following lattice states.
  //
  // Unknown -> HasValue -> Invalidated
  //
  // We treat reinitialization after invalidation as new values.

  // We walk along successor edges depth first. This guarantees that we will
  // visit any dominator of a specific block before we visit that block since
  // any path to the block along successors by definition of dominators we must
  // go through all such dominators.
  BasicBlockWorklist worklist(&*fn->begin());
  llvm::SmallDenseSet<SILDebugVariable, 8> seenDebugVars;

  while (auto *block = worklist.pop()) {
    LLVM_DEBUG(llvm::dbgs()
               << "BB: Visiting. bb" << block->getDebugID() << '\n');
    auto &state = blockToBlockState[block];

    // Then for each inst in the block...
    for (auto &inst : *block) {
      LLVM_DEBUG(llvm::dbgs() << "    Inst: " << inst);
      // If we have a debug_value, store state for it.
      if (auto *dvi = dyn_cast<DebugValueInst>(&inst)) {
        LLVM_DEBUG(llvm::dbgs() << "        Found DebugValueInst!\n");
        auto debugInfo = dvi->getVarInfo();
        if (!debugInfo) {
          LLVM_DEBUG(llvm::dbgs() << "        Has no var info?! Skipping!\n");
          continue;
        }

        // If we have a SILUndef, mark this debug info as being mapped to
        // nullptr.
        if (isa<SILUndef>(dvi->getOperand())) {
          LLVM_DEBUG(llvm::dbgs() << "        SILUndef.\n");
          auto iter = state.lastDebugValues.insert({*debugInfo, nullptr});
          if (!iter.second)
            iter.first->second = nullptr;
          continue;
        }

        // Otherwise, we may have a new debug_value to track. Try to begin
        // tracking it...
        auto iter = state.lastDebugValues.insert({*debugInfo, dvi});

        // If we already have one, we failed to insert... So update the iter
        // by hand. We track the last instance always.
        if (!iter.second) {
          iter.first->second = dvi;
        }
        LLVM_DEBUG(llvm::dbgs() << "        ==> Updated Map.\n");
        continue;
      }

      // Otherwise, check if we have a coroutine boundary non-terminator
      // instruction. If we do, we just dump the relevant debug_value right
      // afterwards.
      if (isInterestingInst(&inst)) {
        LLVM_DEBUG(llvm::dbgs() << "        Found apply edge!.\n");
        // Clone all of the debug_values that we are currently tracking both
        // after the begin_apply,
        SWIFT_DEFER { seenDebugVars.clear(); };

        for (auto &pred : state.lastDebugValues) {
          // If we found a SILUndef, mark this debug var as seen but do not
          // clone.
          if (!pred.second) {
            seenDebugVars.insert(pred.first);
            continue;
          }

          cloneDebugValue(pred.second, &inst);
          // Inside our block, we know that we do not have any repeats since we
          // always track the last debug var.
          seenDebugVars.insert(pred.first);
          madeChange = true;
        }

        // Then walk up the idoms until we reach the entry searching for
        // seenDebugVars.
        madeChange |= propagateDebugValuesFromDominators(
            &inst, inst.getParent(), seenDebugVars);
        continue;
      }

      // Otherwise, we have a yield. We handle this separately since we need to
      // insert the debug_value into its successor blocks.
      if (auto *yi = dyn_cast<YieldInst>(&inst)) {
        LLVM_DEBUG(llvm::dbgs() << "    Found Yield: " << *yi);

        SWIFT_DEFER { seenDebugVars.clear(); };

        // Duplicate all of our tracked debug values into our successor
        // blocks.
        for (auto *succBlock : yi->getSuccessorBlocks()) {
          auto *succBlockInst = &*succBlock->begin();

          LLVM_DEBUG(llvm::dbgs() << "        Visiting Succ: bb"
                                  << succBlock->getDebugID() << '\n');
          for (auto &pred : state.lastDebugValues) {
            if (!pred.second)
              continue;
            LLVM_DEBUG(llvm::dbgs() << "            Cloning: " << *pred.second);
            cloneDebugValue(pred.second, succBlockInst);
            madeChange = true;
          }

          // We start out dataflow in yi, not in inst, even though we use inst
          // as the insert pt. This is b/c inst is in the successor block we
          // haven't processed yet so we would emit any debug_value in the
          // yields own block twice.
          madeChange |= propagateDebugValuesFromDominators(
              succBlockInst, yi->getParent(), seenDebugVars);
        }
      }
    }

    // Now add the block's successor to the worklist if we haven't visited them
    // yet.
    for (auto *succBlock : block->getSuccessorBlocks())
      worklist.pushIfNotVisited(succBlock);
  }

  return madeChange;
#endif
 return false;  
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class DebugInfoCanonicalizerTransform : public SILFunctionTransform {
  void run() override {
    DebugInfoCanonicalizer canonicalizer(getFunction(),
                                         getAnalysis<DominanceAnalysis>(),
                                         getAnalysis<PostOrderAnalysis>());
    if (canonicalizer.process()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createDebugInfoCanonicalizer() {
  return new DebugInfoCanonicalizerTransform();
}
