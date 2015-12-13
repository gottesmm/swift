//===--- ARCLoopHoisting.cpp ----------------------------------------------===//
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
/// This file contains the ARCLoopHoisting algorithm that hoists retains and
/// releases out of loops. This is a high level description that justifies the
/// correction of the algorithm and describes its design. In the following
/// discussion we talk about the algorithm conceptually and show its safety and
/// considerations necessary for good performance.
///
/// *NOTE* In the following when we refer to "hoisting", we are not just talking
/// about upward code motion of retains, but also downward code motion of
/// releases.
///
/// Consider the following simple loop:
///
///   bb0:
///     br bb1
///
///   bb1:
///     retain %x                    (1)
///     apply %f(%x)
///     apply %f(%x)
///     release %x                   (2)
///     cond_br ..., bb1, bb2
///
///   bb2:
///     return ...
///
/// When it is safe to hoist (1),(2) out of the loop? Imagine if we know the
/// trip count of the loop is 3 and completely unroll the loop so the whole
/// function is one basic block. In such a case, we know the function looks
/// as follows:
///
///   bb0:
///     # Loop Iteration 0
///     retain %x
///     apply %f(%x)
///     apply %f(%x)
///     release %x                   (4)
///
///     # Loop Iteration 1
///     retain %x                    (5)
///     apply %f(%x)
///     apply %f(%x)
///     release %x                   (6)
///
///     # Loop Iteration 2
///     retain %x                    (7)
///     apply %f(%x)
///     apply %f(%x)
///     release %x
///
///     return ...
///
/// Notice how (3) can be paired with (4) and (5) can be paired with (6). Assume
/// that we eliminate those. Then the function looks as follows:
///
///   bb0:
///     # Loop Iteration 0
///     retain %x
///     apply %f(%x)
///     apply %f(%x)
///
///     # Loop Iteration 1
///     apply %f(%x)
///     apply %f(%x)
///
///     # Loop Iteration 2
///     apply %f(%x)
///     apply %f(%x)
///     release %x
///
///     return ...
///
/// If we then re-roll the loop, we get the following loop:
///
///   bb0:
///     retain %x                    (8)
///     br bb1
///
///   bb1:
///     apply %f(%x)
///     apply %f(%x)
///     cond_br ..., bb1, bb2
///
///   bb2:
///     release %x                   (9)
///     return ...
///
/// Notice that this transformation is equivalent to just hoisting (1) and (2)
/// out of the loop in the original example. This form of hoisting is what is
/// termed "ARCLoopHoisting". What is key to notice is that even though we are
/// performing "hoisting" we are actually pairing releases from one iteration
/// with retains in the next iteration and then eliminating the pairs. This
/// realization will guide our further analysis.
///
/// In this simple loop case, the proof of correctness is very simple to see
/// conceptually. But in a more general case, when is safe to perform this
/// optimization? We must consider three areas of concern:
///
/// 1. Are the retains/releases upon the same reference count? This can be
///    easily decided by using RCIdentityAnalysis.
///
/// 2. Can we move retains, releases in the unrolled case as we have specified?
///    This is simple since it is always safe to move a retain earlier and a
///    release later in the dynamic execution of a program. This can only extend
///    the life of a variable which is a legal and generally profitable in terms
///    of allowing for this optimization.
///
/// 3. How do we pair all necessary retains/releases to ensure we do not
///    unbalance retain/release counts in the loop? Consider a set of retains
///    and a set of releases that we wish to hoist out of a loop. We can only
///    hoist the retain, release sets out of the loop if all paths in the
///    loop contain exactly one retain and release from the sets.
///
///    For our purposes, we consider exit blocks that are dominated by the loop
///    header but for which the loop header can not be reached apart of our exit
///    release sets.
///
/// Assuming that our optimization does all of these things, we should be able
/// to hoist with safety.
///
/// A final concern that we must consider is if we introduce extra copy on write
/// copies through our optimization. To see this, consider the following simple
/// IR sequence:
///
///   bb0(%0 : $Builtin.NativeObject):
///     // refcount(%0) == n
///     is_unique %0 : $Builtin.NativeObject
///     // refcount(%0) == n
///     strong_retain %0 : $Builtin.NativeObject
///     // refcount(%0) == n+1
///
/// If n is not 1, then trivially is_unique will return false. So assume that n
/// is 1 for our purposes so no copy is occuring here. Thus we have:
///
///   bb0(%0 : $Builtin.NativeObject):
///     // refcount(%0) == 1
///     is_unique %0 : $Builtin.NativeObject
///     // refcount(%0) == 1
///     strong_retain %0 : $Builtin.NativeObject
///     // refcount(%0) == 2
///
/// Now imagine that we move the strong_retain before the is_unique. Then we
/// have:
///
///   bb0(%0 : $Builtin.NativeObject):
///     // refcount(%0) == 1
///     strong_retain %0 : $Builtin.NativeObject
///     // refcount(%0) == 2
///     is_unique %0 : $Builtin.NativeObject
///
/// Thus is_unique is guaranteed to return false introducing a copy that was not
/// needed.
///
/// A similar issue can occur when one sinks releases. In order to avoid this
/// issue, we stop code motion whenever we can not prove that we are moving a
/// retain/release over a uniqueness check.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-arc-loop-hoisting"
#include "ARCLoopHoisting.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/LoopRegionAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/Analysis/SideEffectAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/LoopUtils.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"

using namespace swift;

STATISTIC(NumLoopHoisting, "Number of times a retain, release set was hoisted");
STATISTIC(NumHoistedRetains, "Number of retains hoisted");
STATISTIC(NumHoistedReleases, "Number of releases hoisted");

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

static bool isDecrement(const SILInstruction &I) {
  return isa<StrongReleaseInst>(I) || isa<ReleaseValueInst>(I);
}

static bool isIncrement(const SILInstruction &I) {
  return isa<StrongRetainInst>(I) || isa<RetainValueInst>(I);
}

// For now do not use side effect analysis and just assume that is_unique and
// all full apply sites can be a uniqueness check. This will be replaced by side
// effect analysis.
static bool isUniquenessCheck(const SILInstruction &I) {
  return isa<FullApplySite>(I) || isa<IsUniqueInst>(I);
}

//===----------------------------------------------------------------------===//
//                           RCIdentityToInstSetMap
//===----------------------------------------------------------------------===//

void RCIdentityToInstSetMap::clear() {
  for (auto &P : Insts) {
    for (auto *I : P.second) {
      DEBUG(llvm::dbgs() << "    Adding to unhoistable set: " << *I);
      UnhoistableInsts.insert(I);
    }
  }
  Insts.clear();
}

void RCIdentityToInstSetMap::reset() { Insts.clear(); }

// TODO: This should be made more efficient.
bool RCIdentityToInstSetMap::erase(SILValue V) {
  auto Iter = find(V);
  if (Iter == end())
    return false;
  for (auto *I : Iter->second) {
    DEBUG(llvm::dbgs() << "    Adding to unhoistable set: " << *I);
    UnhoistableInsts.insert(I);
  }
  Insts.erase(V);
  return true;
}

void RCIdentityToInstSetMap::print(llvm::raw_ostream &os) {
  for (auto &P : Insts) {
    llvm::dbgs() << "        RCID: " << P.first;
    for (auto *I : P.second) {
      bool isHoistable = !UnhoistableInsts.count(I);
      llvm::dbgs() << "            CanHoist: " << (isHoistable ? "yes" : "no ")
                   << " Inst: " << *I;
    }
  }
}

void RCIdentityToInstSetMap::dump() { DEBUG(print(llvm::dbgs())); }

//===----------------------------------------------------------------------===//
//                                 LHContext
//===----------------------------------------------------------------------===//

LHContext::LHContext(LoopRegionFunctionInfo *LRFI, RCIdentityFunctionInfo *RCFI)
    : Allocator(), RegionIDToStateMap(), LRFI(LRFI), RCFI(RCFI) {
  for (auto *R : LRFI->getRegions()) {
    RegionIDToStateMap.push_back(new (Allocator) LHRegionState(
        R, UnhoistableRetains, UnhoistableReleases));
  }
}

LHContext::~LHContext() {
  for (auto *R : RegionIDToStateMap) {
    R->~LHRegionState();
  }
}

//===----------------------------------------------------------------------===//
//                             Top Down Dataflow
//===----------------------------------------------------------------------===//

void LHRegionState::performTopDownMerge(LHContext &State) {
  Releases.reset();
  GenRetains.clear();

  // TODO: Remove this clear it should not be necessary.
  UniquenessChecks.clear();

  DEBUG(llvm::dbgs() << "Performing top down merge for: " << R->getID()
                     << "\n");
  auto Preds = R->getPreds();
  auto PI = Preds.begin(), PE = Preds.end();
  if (PI == PE) {
    DEBUG(llvm::dbgs() << "    No predecessors... early returning.\n");
    return;
  }

  Releases = State.RegionIDToStateMap[*PI]->Releases;
  DEBUG(llvm::dbgs() << "    Initializing region with pred " << *PI << "\n");
  DEBUG(Releases.dump());

  ++PI;

  llvm::SmallVector<SILValue, 2> ValuesToDelete;
  llvm::SmallVector<SILInstruction *, 4> InstsToAdd;
  for (; PI != PE; ++PI) {
    auto &PState = *State.RegionIDToStateMap[*PI];

    for (auto &P : Releases) {
      auto Iter = PState.Releases.find(P.first);
      if (Iter == PState.Releases.end()) {
        ValuesToDelete.push_back(P.first);
        continue;
      }

      for (auto *I : Iter->second) {
        P.second.insert(I);
      }
    }

    // Mark as unhoistable any releases associated with a SILValue in PStates
    // for a SILValue that we do not have in P.
    for (auto &P : PState.Releases) {
      auto Iter = Releases.find(P.first);
      if (Iter != Releases.end()) {
        continue;
      }

      // We need to make sure that values that we can not merge are never
      // hoisted. So even though we know it is not in releases, we abuse its
      for (auto *I : P.second) {
        DEBUG(llvm::dbgs() << "    Marking unhoistable: " << *I);
        State.UnhoistableReleases.insert(I);
      }
    }

    DEBUG(llvm::dbgs() << "    After merging pred " << *PI << "\n");
    DEBUG(Releases.dump());
  }

  for (SILValue V : ValuesToDelete) {
    DEBUG(llvm::dbgs() << "    Removing value: " << V);
    Releases.erase(V);
  }

  DEBUG(llvm::dbgs() << "    After merging: \n");
  DEBUG(Releases.dump());
}

void LHRegionState::performTopDownDataflow(LHContext &State) {
  performTopDownMerge(State);

  // We can only hoist releases through this block if it is not an unknown
  // control flow edge tail. If it is an unknown control flow edge tail, we need
  // to not propagate the releases through and then black list the releases a
  // being unhoistable. We can still hoist any releases that are generated by
  // this block though, so we do not exit early.
  if (R->isUnknownControlFlowEdgeHead()) {
    DEBUG(llvm::dbgs() << "    Can not hoist pred state through block. "
                          "Clearing state before dataflow.\n");
    Releases.clear();
  }

  // Now perform the dataflow for this block.
  if (R->isBlock()) {
    performTopDownBlockDataflow(State);
  } else {
    performTopDownLoopDataflow(State);
  }

  DEBUG(llvm::dbgs() << "    After Top Down Dataflow:\n");
  DEBUG(Releases.dump());

  // Finally, determine if we can hoist releases from this block and if we can
  // not do so, mark all of the releases that we are currently tracking as being
  // unhoistable. We can only hoist releases the region is not an unknown
  // control flow edge tail.
  //
  // Unlike the case where we are processing bottom up, we can ignore the
  // possibility of having non-local successors/local successors since if this
  // release is not able to hit all other exit blocks from this point (due to us
  // tracking a different release or a dataflow merge failure), we will have
  // added the release to the unhoistable release set.
  if (!R->isUnknownControlFlowEdgeTail()) {
    return;
  }

  // Otherwise, we need to clear all releases we are tracking and mark them as
  // unhoistable so we are conservatively correct.
  DEBUG(llvm::dbgs() << "    Can't hoist! Clearing state!\n");
  Releases.clear();
}

void LHRegionState::performTopDownBlockDataflow(LHContext &State) {
  // For a block we perform the following work:
  //
  // 1. We merge any releases that are being tracked by our predecessors into
  // our release list.
  //
  // 2. We perform the top down dataflow. While doing this we prepare for the
  // bottom up dataflow ensuring we do not visit instructions twice. With that
  // in mind:
  //
  //    a. Any retains we see, we immediately add to our tracked retain
  //       list. After we see a potential uniqueness check, we stop adding
  //       retains.
  //
  //    b. Any releases we see, we immediately add to our tracked release
  //       list. We clear the release list whenever we see a potential
  //       uniqueness check.
  for (auto &II : *R->getBlock()) {
    if (UniquenessChecks.empty() && isIncrement(II)) {
      GenRetains.push_back(&II);
    }

    if (isDecrement(II)) {
      SILValue V = State.RCFI->getRCIdentityRoot(II.getOperand(0));
      Releases.erase(V);
      auto &R = Releases[V];
      R.insert(&II);
    }

    if (isUniquenessCheck(II)) {
      UniquenessChecks.push_back(&II);
      Releases.clear();
    }
  }
}

void LHRegionState::performTopDownLoopDataflow(LHContext &State) {
  // For now if we have any uniqueness checks, clear any tracked releases.
  if (UniquenessChecks.empty())
    return;
  Releases.clear();
}

//===----------------------------------------------------------------------===//
//                             Bottom Up Dataflow
//===----------------------------------------------------------------------===//

void LHRegionState::performBottomUpMerge(LHContext &State) {
  Retains.reset();

  DEBUG(llvm::dbgs() << "Performing bottom up merge for: " << R->getID()
                     << "\n");

  if (!R->hasLocalSuccs()) {
    DEBUG(llvm::dbgs() << "    Region has no local successors, initializing "
                          "with empty state and returning.\n");
    return;
  }

  auto Succs = R->getLocalSuccs();
  auto SI = Succs.begin(), SE = Succs.end();

  Retains = State.RegionIDToStateMap[*SI]->Retains;
  DEBUG(llvm::dbgs() << "    Initializing region with succ " << *SI << "\n");
  DEBUG(Retains.dump());
  ++SI;

  llvm::SmallVector<SILValue, 2> ValuesToDelete;
  for (; SI != SE; ++SI) {
    auto &PState = *State.RegionIDToStateMap[*SI];
    for (auto &P : Retains) {
      auto Iter = PState.Retains.find(P.first);
      if (Iter == PState.Retains.end()) {
        ValuesToDelete.push_back(P.first);
        continue;
      }

      for (auto *I : Iter->second) {
        P.second.insert(I);
      }
    }

    // Mark as unhoistable any retains associated with a SILValue in PStates for
    // a SILValue that we do not have in P.
    for (auto &P : PState.Retains) {
      auto Iter = Retains.find(P.first);
      if (Iter != Retains.end()) {
        continue;
      }

      // We need to make sure that values that we can not merge are never
      // hoisted. So even though we know it is not in releases, we abuse its
      for (auto *I : P.second) {
        DEBUG(llvm::dbgs() << "    Marking unhoistable: " << *I);
        State.UnhoistableRetains.insert(I);
      }
    }

    DEBUG(llvm::dbgs() << "    After merging succ " << *SI << "\n");
    DEBUG(Retains.dump());
  }

  for (SILValue V : ValuesToDelete) {
    DEBUG(llvm::dbgs() << "    Removing value: " << V);
    Retains.erase(V);
  }

  DEBUG(llvm::dbgs() << "    After merging: \n");
  DEBUG(Retains.dump());
}

void LHRegionState::performBottomUpDataflow(LHContext &State) {
  // First perform the bottom up merge.
  performBottomUpMerge(State);

  // Then check if we can hoist any of the merged retains. We can only hoist
  // retains from other blocks through this block if the block:
  //
  // 1. Is not an unknown control flow edge tail.
  // 2. This block does not have
  //
  // We may still be able to hoist retains generated by this block so perform
  // the actual dataflow.
  if (R->isUnknownControlFlowEdgeTail() ||
      (R->hasNonLocalSuccs() && R->hasLocalSuccs())) {
    DEBUG(llvm::dbgs()
          << "    Can not hoist succ state through block. Clearing state.\n");
    Retains.clear();
  }

  if (R->isBlock()) {
    performBottomUpBlockDataflow(State);
  } else {
    performBottomUpLoopDataflow(State);
  }

  DEBUG(llvm::dbgs() << "    After Bottom Up Dataflow:\n");
  DEBUG(Retains.dump());

  // If this block is an unknown control flow edge head, we can not propagate
  // retains into its predecessors. If we are not an unknown control flow edge
  // head, return. Otherwise, clear all retains and mark them as unhoistable.
  if (!R->isUnknownControlFlowEdgeHead()) {
    return;
  }

  DEBUG(llvm::dbgs() << "    Unknown control flow edge head, clearing "
                        "state.\n");
  Retains.clear();
}

void LHRegionState::performBottomUpBlockDataflow(LHContext &State) {
  DEBUG(llvm::dbgs() << "Performing bottom up dataflow for: " << R->getID()
                     << "\n");
  // For the bottom up dataflow, we have already found all retains that reach
  // the bottom of this block without hitting a uniqueness check. The retains
  // are in top down order, meaning that in order to ensure that we only
  for (auto *Retain : GenRetains) {
    DEBUG(llvm::dbgs() << "    Visiting GenRetain: " << *Retain);
    SILValue V = State.RCFI->getRCIdentityRoot(Retain->getOperand(0));
    Retains.erase(V);
    auto &R = Retains[V];
    R.insert(Retain);
  }
}

void LHRegionState::performBottomUpLoopDataflow(LHContext &State) {
  // For a loop, we just want to clear any retains that we are tracking that
  // could have a uniqueness check applied to them. For now we just always bail
  // in such cases.
  if (UniquenessChecks.empty())
    return;
  Retains.clear();
}

//===----------------------------------------------------------------------===//
//                              Summarize Loops
//===----------------------------------------------------------------------===//

void LHRegionState::summarize(LHContext &State) {
  assert(!R->isBlock() && "Can not summarize a block");

  // All we do in order to summarize is gather up all of the uniqueness checks
  // from subregions.
  for (unsigned SubregionID : R->getSubregions()) {
    auto &Checks = State.RegionIDToStateMap[SubregionID]->UniquenessChecks;
    std::copy(Checks.begin(), Checks.end(),
              std::back_inserter(UniquenessChecks));
  }
}

//===----------------------------------------------------------------------===//
//                                Loop Hoister
//===----------------------------------------------------------------------===//

void LoopHoister::runOnLoop(SILLoop *L) {
  runOnLoopRegion(Ctx.LRFI->getRegion(L));
}

void LoopHoister::runOnLoopRegion(LoopRegion *R) {
  assert(!R->isBlock() && "Expecting a non-block region");

  // If R has any non-local successors, do not do anything. We do not support
  // hoisting retains/releases through multiple levels of the loop nest
  // hierarchy yet.
  if (distance(R->getNonLocalSuccs()) != 0) {
    return;
  }

  // We process a loop as follows:
  //
  // 1. We begin by performing a top down dataflow discovering our gen retain,
  // gen release sets. Additionally, we determine the releases that we can sink
  // to exits. In this dataflow, we visit instructions.
  //
  // 2. Then we perform a bottom up dataflow using summarized data from the top
  // down dataflow. This just ensures that we do not need to touch instructions
  // again to save compile time.
  //
  // 3. Any set of retains that we can hoist to our entry block and any releases
  // that we can sink to our exit blocks are hoisted out of the loop.
  //
  // 4. We summarize our uniqueness check list into this loop for the next loop
  // level to use.
  bool Changed = false;
  do {
    performTopDownDataflow(R);
    performBottomUpDataflow(R);
    Changed = pairIncrementsDecrements(R);
  } while (Changed);
  Ctx.RegionIDToStateMap[R->getID()]->summarize(Ctx);
  DEBUG(llvm::dbgs() << "\n");
}

// We can not hoist out of functions, so we do not do anything beyond make the
// output pretty.
void LoopHoister::runOnFunction(SILFunction *F) { DEBUG(llvm::dbgs() << "\n"); }

void LoopHoister::performTopDownDataflow(LoopRegion *R) {
  for (unsigned SubregionID : R->getSubregions()) {
    auto *Subregion = Ctx.LRFI->getRegion(SubregionID);
    Ctx.RegionIDToStateMap[Subregion->getID()]->performTopDownDataflow(Ctx);
  }
}

void LoopHoister::performBottomUpDataflow(LoopRegion *R) {
  // TODO: Figure out why this is segfaulting if we use a reverse_iterator.
  auto Range = R->getSubregions();
  auto II = Range.begin(), IE = Range.end();

  if (II == IE)
    return;

  --IE;
  for (; II != IE; --IE) {
    unsigned SubregionID = *IE;
    auto *Subregion = Ctx.LRFI->getRegion(SubregionID);
    Ctx.RegionIDToStateMap[Subregion->getID()]->performBottomUpDataflow(Ctx);
  }

  {
    unsigned SubregionID = *II;
    auto *Subregion = Ctx.LRFI->getRegion(SubregionID);
    Ctx.RegionIDToStateMap[Subregion->getID()]->performBottomUpDataflow(Ctx);
  }
}

static void createIncrement(SILBuilder &B, SILValue Op) {
  auto Loc = SILFileLocation(SourceLoc());
  if (Op.getType().hasReferenceSemantics()) {
    B.createStrongRetain(Loc, Op);
    return;
  }

  B.createRetainValue(Loc, Op);
}

static void createDecrement(SILBuilder &B, SILValue Op) {
  auto Loc = SILFileLocation(SourceLoc());
  if (Op.getType().hasReferenceSemantics()) {
    B.createStrongRelease(Loc, Op);
    return;
  }
  B.createReleaseValue(Loc, Op);
}

namespace {
struct HoistableSets {
  llvm::SmallPtrSet<SILInstruction *, 1> Retains;
  llvm::SmallPtrSet<SILInstruction *, 1> Releases;
};
} // end anonymous namespace

static void findHoistableIncrements(
    const RCIdentityToInstSetMap &DataflowResult,
    llvm::SmallDenseMap<SILValue, HoistableSets, 4> &HoistableSets,
    llvm::SmallPtrSet<SILInstruction *, 4> &UnhoistableIncrements,
    llvm::SmallPtrSet<SILValue, 4> &UnhoistableValues) {
  for (const auto &P : DataflowResult) {
    if (std::any_of(P.second.begin(), P.second.end(),
                    [&UnhoistableIncrements](SILInstruction *I)
                        -> bool { return UnhoistableIncrements.count(I); })) {
      UnhoistableValues.insert(P.first);
      continue;
    }
    HoistableSets[P.first] = {P.second, {}};
  }
}

static bool findHoistableDecrementsForValue(
    SILValue V, HoistableSets &HSets, ArrayRef<LHRegionState *> ExitStates,
    llvm::SmallPtrSet<SILInstruction *, 4> &UnhoistableDecrements) {

  // For each exiting block state...
  for (auto *State : ExitStates) {

    // Attempt to find a set of releases associated with V. If we can not find
    // one, then we can not hoist increments/decrements for v, so return false.
    const auto &Releases = State->getReleases();
    auto Iter = Releases.find(V);
    if (Iter == Releases.end()) {
      HSets.Releases.clear();
      return false;
    }

    // Next make sure that none of the decrements are in the unhoistable
    // decrements set. If one of the decrements are in the unhoistable set, then
    // we know along at least one path through the loop, this decrement was
    // forgotten in favor of a later release. If we were to hoist such a
    // release, then along the aforementioned path we would be removing 2
    // dynamic release operations instead of 1.
    if (std::any_of(Iter->second.begin(), Iter->second.end(),
                    [&UnhoistableDecrements](SILInstruction *I)
                        -> bool { return UnhoistableDecrements.count(I); })) {
      HSets.Releases.clear();
      return false;
    }

    // This is an exiting block which we can use for our pairing. Add its
    // releases to the HoistableSet.
    HSets.Releases.insert(Iter->second.begin(), Iter->second.end());
  }

  // We can pair! Return true.
  return true;
}

static void findHoistableDecrements(
    LoopRegion *R, const RCIdentityToInstSetMap &DataflowResult,
    llvm::SmallDenseMap<SILValue, HoistableSets, 4> &HoistableSets,
    LHContext &Ctx, llvm::SmallPtrSet<SILValue, 4> &UnhoistableValues) {

  // Gather all of the LHRegionState for each loop exit.
  //
  // It is very unlikely that we will have more than 8 exits from a loop so this
  // is a safe small vector size to assume.
  llvm::SmallVector<LHRegionState *, 8> ExitStates;
  for (unsigned ID : R->getExitingSubregions()) {
    ExitStates.push_back(Ctx.RegionIDToStateMap[ID]);
  }

  // For each SILValue...
  for (auto &P : HoistableSets) {
    // If we already know that we can not hoist retains/releases on the
    // SILValue, just skip it.
    if (UnhoistableValues.count(P.first))
      continue;

    // Then see if we have release sets for each exit block and that these
    // release sets do not contain unhoistable release instructions. If we do
    // not, then we know that this value is not hoistable. Bail.
    if (findHoistableDecrementsForValue(P.first, P.second, ExitStates,
                                        Ctx.UnhoistableReleases))
      continue;
    UnhoistableValues.insert(P.first);
  }
}

static void markInvertedOrderSetsUnhoistable(
    llvm::SmallDenseMap<SILValue, HoistableSets, 4> &HoistableSets,
    LHContext &Ctx, llvm::SmallPtrSet<SILValue, 4> &UnhoistableValues) {

  using InstStatePair = std::pair<SILInstruction *, LHRegionState *>;
  llvm::SmallVector<InstStatePair, 4> RetainStates;
  llvm::SmallVector<InstStatePair, 4> ReleaseStates;

  // For each SILValue...
  for (auto &P : HoistableSets) {
    // If we already know that we can not hoist retains/releases on the
    // SILValue, just skip it.
    if (UnhoistableValues.count(P.first)) {
      continue;
    }

    // This needs to be made more efficient b/c it is O(N)^2. In general our N
    // should be relatively low since loops do not have /that/ many exits. But
    // if it is an issue, we can probably cache this information as we perform
    // the dataflow.
    for (auto *I : P.second.Retains) {
      unsigned RegionID = Ctx.LRFI->getRegion(I->getParent())->getID();
      RetainStates.push_back({I, Ctx.RegionIDToStateMap[RegionID]});
    }

    for (auto *I : P.second.Releases) {
      unsigned RegionID = Ctx.LRFI->getRegion(I->getParent())->getID();
      ReleaseStates.push_back({I, Ctx.RegionIDToStateMap[RegionID]});
    }

    for (auto RetainP : RetainStates) {
      for (auto ReleaseP : ReleaseStates) {
        SILBasicBlock *RetainParent = RetainP.first->getParent();

        // If the retain, release are in different blocks, see if one or the
        // other reached the block.
        if (RetainParent != ReleaseP.first->getParent()) {
          if (!RetainP.second->hasReleaseForValue(P.first, ReleaseP.first) &&
              !ReleaseP.second->hasRetainForValue(P.first, RetainP.first)) {
            continue;
          }
          UnhoistableValues.insert(P.first);
          return;
        }

        // If the retain, release ar ein the same block, make sure that the
        // release is /after/ the retain.
        if (std::find_if(RetainP.first->getIterator(), RetainParent->end(),
                         [&ReleaseP](const SILInstruction &I) -> bool {
                           return &I == ReleaseP.first;
                         }) != RetainParent->end()) {
          continue;
        }

        // If we can not prove any of these things, we can not hoist this
        // retain.
        UnhoistableValues.insert(P.first);
        return;
      }
    }
  }
}

bool LoopHoister::pairIncrementsDecrements(LoopRegion *R) {

  // Look at the entrances and exits of R and see if we can find any retains,
  // releases that we can hoist.
  llvm::SmallDenseMap<SILValue, HoistableSets, 4> Values;
  llvm::SmallPtrSet<SILValue, 4> UnhoistableValues;

  unsigned LoopHeaderID = *R->getSubregions().begin();
  auto *State = Ctx.RegionIDToStateMap[LoopHeaderID];
  findHoistableIncrements(State->getRetains(), Values, Ctx.UnhoistableRetains,
                          UnhoistableValues);

  // If we did not find any values that we /could/ hoist, return early.
  if (Values.empty()) {
    Ctx.UnhoistableRetains.clear();
    Ctx.UnhoistableReleases.clear();
    return false;
  }

  // Otherwise, attempt to pair the increments that we decided could be hoisted
  // with decrements.
  findHoistableDecrements(R, State->getReleases(), Values, Ctx,
                          UnhoistableValues);

  // If we do not have any values that we can hoist, there is nothing further to
  // do, bail.
  if (UnhoistableValues.size() == Values.size()) {
    Ctx.UnhoistableRetains.clear();
    Ctx.UnhoistableReleases.clear();
    return false;
  }

  // Make sure that our retain/release sets are not inverted in order. We can
  // prove this by noting that our retain set must joint-dominate our release
  // set and vis-a-versa. This means that to prove this property (without any
  // loss of generality considering just retains), we just have to prove for
  // each release that either the release is not in the same block and is not in
  // the release set for that block or if the release is in the same block that
  // their order is ok.
  markInvertedOrderSetsUnhoistable(Values, Ctx, UnhoistableValues);

  // If we do not have any values that we can hoist, there is nothing further to
  // do, bail.
  if (UnhoistableValues.size() == Values.size()) {
    Ctx.UnhoistableRetains.clear();
    Ctx.UnhoistableReleases.clear();
    return false;
  }

  // Ok, we have some retains, releases that we can hoist!
  ++NumLoopHoisting;

  assert((std::distance(R->pred_begin(), R->pred_end()) == 1) &&
         "Expected all headers to have a loop pre-header");
  auto *PreheaderState = Ctx.RegionIDToStateMap[*R->pred_begin()];
  SILBuilder PredBuilder(
      PreheaderState->getRegion()->getBlock()->getTerminator());
  llvm::SmallVector<SILBuilder, 4> SuccBuilders;

  // By assumption at this point we know that we do not have any non-local
  // successors so all of our successors are local. We additionally know that
  // all successors must be blocks due to our loop canonicalization.
  for (unsigned SuccID : R->getLocalSuccs()) {
    auto *SuccState = Ctx.RegionIDToStateMap[SuccID];
    auto *InsertPt = SuccState->getRegion()->getBlock()->getTerminator();
    SuccBuilders.push_back(SILBuilder(InsertPt));
  }

  for (auto &P : Values) {
    if (UnhoistableValues.count(P.first))
      continue;

    DEBUG(llvm::dbgs() << "Hoisting:\n");
    DEBUG(for (auto *Retain
               : P.second.Retains) { llvm::dbgs() << "    " << *Retain; });
    DEBUG(for (auto *Release
               : P.second.Releases) { llvm::dbgs() << "    " << *Release; });
    createIncrement(PredBuilder, P.first);
    for (auto *Retain : P.second.Retains) {
      Retain->eraseFromParent();
      ++NumHoistedRetains;
    }
    P.second.Retains.clear();

    for (auto &SuccBuilder : SuccBuilders) {
      createDecrement(SuccBuilder, P.first);
    }
    for (auto *Release : P.second.Releases) {
      Release->eraseFromParent();
      ++NumHoistedReleases;
    }
    P.second.Releases.clear();
  }

  Ctx.UnhoistableRetains.clear();
  Ctx.UnhoistableReleases.clear();
  MadeChange = true;
  return true;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class ARCLoopHoisting : public SILFunctionTransform {
  void run() override {
    if (!getOptions().EnableARCOptimizations)
      return;

    // Canonicalize the loops, invalidating if we need to.
    auto *LA = getAnalysis<SILLoopAnalysis>();
    auto *LI = LA->get(getFunction());
    auto *DA = getAnalysis<DominanceAnalysis>();
    auto *DI = DA->get(getFunction());

    if (canonicalizeAllLoops(DI, LI)) {
      // We preserve loop info and the dominator tree.
      DA->lockInvalidation();
      LA->lockInvalidation();
      PM->invalidateAnalysis(getFunction(),
                             SILAnalysis::InvalidationKind::FunctionBody);
      DA->unlockInvalidation();
      LA->unlockInvalidation();
    }

    auto *RCFI = getAnalysis<RCIdentityAnalysis>()->get(getFunction());
    auto *LRFI = getAnalysis<LoopRegionAnalysis>()->get(getFunction());

    LoopHoister L(getFunction(), LI, RCFI, LRFI);
    L.run();
    if (L.madeChange()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }

  StringRef getName() override { return "ARC Loop Hoisting"; }
};

} // end anonymous namespace

SILTransform *swift::createARCLoopHoisting() { return new ARCLoopHoisting(); }
