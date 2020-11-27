//===--- BasicBlockUtils.h - Utilities for SILBasicBlock  -------*- C++ -*-===//
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

#ifndef SWIFT_SIL_BASICBLOCKUTILS_H
#define SWIFT_SIL_BASICBLOCKUTILS_H

#include "swift/SIL/SILValue.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class SILFunction;
class SILBasicBlock;
class TermInst;
class DominanceInfo;
class SILLoopInfo;

/// Replace a branch target.
///
/// \param T The terminating instruction to modify.
/// \param edgeIdx The successor edges index that will be replaced.
/// \param newDest The new target block.
/// \param preserveArgs If set, preserve arguments on the replaced edge.
void changeBranchTarget(TermInst *T, unsigned edgeIdx, SILBasicBlock *newDest,
                        bool preserveArgs);

/// Returns the arguments values on the specified CFG edge. If necessary, may
/// add create new SILPHIArguments, using `NewEdgeBB` as the placeholder.
void getEdgeArgs(TermInst *T, unsigned edgeIdx, SILBasicBlock *newEdgeBB,
                 llvm::SmallVectorImpl<SILValue> &args);

/// Splits the edge from terminator.
///
/// Also updates dominance and loop information if not null.
///
/// Returns the newly created basic block.
SILBasicBlock *splitEdge(TermInst *T, unsigned edgeIdx,
                         DominanceInfo *DT = nullptr,
                         SILLoopInfo *LI = nullptr);

/// Merge a basic block ending in a branch with its successor
/// if possible.
void mergeBasicBlockWithSingleSuccessor(SILBasicBlock *BB,
                                        SILBasicBlock *succBB);

/// A utility for finding dead-end blocks.
///
/// Dead-end blocks are blocks from which there is no path to the function exit
/// (either return or throw). These are blocks which end with an unreachable
/// instruction and blocks from which all paths end in "unreachable" blocks.
/// This utility is needed to determine if the a value definition can have a
/// lack of users ignored along a specific path.
class DeadEndBlocks {
  llvm::SetVector<const SILBasicBlock *> ReachableBlocks;
  const SILFunction *F;
  bool isComputed = false;

  void compute();

public:
  DeadEndBlocks(const SILFunction *F) : F(F) {}

  /// Returns true if \p BB is a dead-end block.
  bool isDeadEnd(const SILBasicBlock *block) {
    if (!isComputed) {
      // Lazily compute the dataflow.
      compute();
      isComputed = true;
    }
    return ReachableBlocks.count(block) == 0;
  }

  bool empty() {
    if (!isComputed) {
      // Lazily compute the dataflow.
      compute();
      isComputed = true;
    }
    return ReachableBlocks.empty();
  }
};

/// A struct that contains the intermediate state used in computing
/// joint-dominance sets. Enables a pass to easily reuse the same small data
/// structures with clearing (noting that clearing our internal state does not
/// cause us to shrink meaning that once we malloc, we keep the malloced
/// memory).
struct JointPostDominanceSetComputer {
  SmallVector<SILBasicBlock *, 32> worklist;
  SmallPtrSet<SILBasicBlock *, 32> visitedBlocks;
  SmallPtrSet<SILBasicBlock *, 8> initialBlocks;

  /// As we process the worklist, any successors that we see that have not been
  /// visited yet are placed in here. At the end of our worklist, any blocks
  /// that remain here are "leaking blocks" that together with our initial set
  /// would provide a jointly-postdominating set of our dominating value.
  SmallSetVector<SILBasicBlock *, 8> blocksThatLeakIfNeverVisited;

  DeadEndBlocks &deadEndBlocks;

  JointPostDominanceSetComputer(DeadEndBlocks &deadEndBlocks)
      : deadEndBlocks(deadEndBlocks) {}

  void clear() {
    worklist.clear();
    visitedBlocks.clear();
    initialBlocks.clear();
    blocksThatLeakIfNeverVisited.clear();
  }

  /// Let \p dominatingBlock be a dominating block and \p
  /// partialPostDominatingSet a set of blocks, dominated by dominating block,
  /// that partially post-dominate \p dominating block. Given those inputs, this
  /// function attempts to compute a valid set of blocks that
  /// jointly-postdominate \p dominatingBlock and also jointly post-dominate our
  /// \p partialPostDominatingSet blocks.
  ///
  /// To do this we handle two cases based off of whether or not the blocks in
  /// \p partialPostDomSet are all in the same level of the loop nest.
  ///
  /// 1. (Different Loop Nest Level). If while walking backwards we run into any
  ///    of our partial post dominating set elements, then we know that we have
  ///    found a backedge of some sort and some of our partial post dominating
  ///    set elements are in different loops within the loop nest. In such a
  ///    case, we pass to \p foundPostDomBlockCallback instead the loop exit
  ///    blocks and pass to \p foundLoopBlockCallback the block that was found
  ///    to be reachable from one of the other blocks. The key thing to note
  ///    here is that we are not "completing" the partial post dom set.
  //
  /// 2. (Same Loop Nest Level) If all elements of \p partialPostDominatingSet
  /// are
  ///    in the same loop (that is we don't find any backedges while processing)
  ///    then this returns the set of blocks that combined with \p
  ///    partialPostDominatingSet post-dominate \p dominatingBlock. These are
  ///    returned by calling foundPostDomBlockCallback.
  ///
  /// The way to work with the two cases is to see if one gathered any
  /// foundLoopBlockCallback.
  ///
  /// NOTE: We ignore paths through dead end blocks.
  void findJointPostDominatingSet(
      SILBasicBlock *dominatingBlock,
      ArrayRef<SILBasicBlock *> partialPostDomSet,
      function_ref<void(SILBasicBlock *foundPostDomBlock)> resultCallback,
      function_ref<void(SILBasicBlock *foundLoopBlock)> foundLoopBlockCallback);
};

} // namespace swift

#endif
