//===--- ARCLoopHoisting.h ------------------------------------------------===//
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

#ifndef SWIFT_SILPASSES_ARC_ARCLOOPHOISTING_H
#define SWIFT_SILPASSES_ARC_ARCLOOPHOISTING_H

#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Utils/LoopUtils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"

namespace swift {

class LoopRegion;
class SILInstruction;
class LoopRegionFunctionInfo;
class RCIdentityFunctionInfo;
struct LHContext;

/// A wrapper around a map from SILValue -> SmallPtrSet<SILInstruction *> that
/// ensures that the unhoistable inst sets are updated appropriately when values
/// are deleted from the map.
class RCIdentityToInstSetMap {
public:
  using SetTy = llvm::SmallPtrSet<SILInstruction *, 1>;

private:
  using InnerMapTy = llvm::SmallDenseMap<SILValue, SetTy, 4>;

  /// A map from a specific RCIdentity to the set of hoistable instructions on
  /// that RCIdentity that reach this block.
  InnerMapTy Insts;

  /// A reference to a global set of unhoistable instructions. Whenever we clear
  /// Insts, we need to update the set with the contents of Insts so we know not
  /// to hoist the instruction if along a different path it reaches a loop
  /// entrance/loop exit.
  llvm::SmallPtrSet<SILInstruction *, 4> &UnhoistableInsts;

public:
  RCIdentityToInstSetMap(
      llvm::SmallPtrSet<SILInstruction *, 4> &unhoistableinsts)
      : UnhoistableInsts(unhoistableinsts) {}
  RCIdentityToInstSetMap(const RCIdentityToInstSetMap &Map)
      : Insts(Map.Insts), UnhoistableInsts(Map.UnhoistableInsts) {}
  RCIdentityToInstSetMap &operator=(const RCIdentityToInstSetMap &Map) {
    Insts = Map.Insts;
    UnhoistableInsts = Map.UnhoistableInsts;
    return *this;
  }

  using iterator = decltype(Insts)::iterator;
  using const_iterator = decltype(Insts)::const_iterator;

  iterator begin() { return Insts.begin(); }
  iterator end() { return Insts.end(); }
  const_iterator begin() const { return Insts.begin(); }
  const_iterator end() const { return Insts.end(); }

  void clear();

  void reset();

  iterator find(SILValue V) { return Insts.find(V); }
  const_iterator find(SILValue V) const { return Insts.find(V); }

  bool erase(SILValue V);

  SetTy &operator[](SILValue V) { return Insts[V]; }

  void dump();
  void print(llvm::raw_ostream &os);
};

/// A data structure representing the current tracked state for a region.
class LHRegionState {
  /// The region that this state is matched to.
  LoopRegion *R;

  /// A list of instructions in this block that /may/ perform uniqueness check
  /// operations.
  llvm::SmallVector<SILInstruction *, 2> UniquenessChecks;

  /// The set of generated retains in the block discovered during the top down
  /// traversal. We use this during the bottom up traversal so that we do not
  /// need to revisit instructions.
  llvm::SmallVector<SILInstruction *, 2> GenRetains;

  /// A map from a specific RCIdentity to the set of hoistable releases on that
  /// RCIdentity that reach this block.
  RCIdentityToInstSetMap Retains;

  /// A map from a specific RCIdentity to the set of hoistable releases on that
  /// RCIdentity that reach this block.
  RCIdentityToInstSetMap Releases;

public:
  LHRegionState(LoopRegion *Region,
                llvm::SmallPtrSet<SILInstruction *, 4> &UnhoistableRetains,
                llvm::SmallPtrSet<SILInstruction *, 4> &UnhoistableReleases)
      : R(Region), UniquenessChecks(), GenRetains(),
        Retains(UnhoistableRetains), Releases(UnhoistableReleases) {}
  ~LHRegionState() = default;

  LHRegionState(const LHRegionState &) = delete;
  LHRegionState(LHRegionState &&) = delete;

  void performTopDownDataflow(LHContext &State);
  void performBottomUpDataflow(LHContext &State);
  void summarize(LHContext &State);

  /// Returns true if \p Retain is an increment associated with \p V at the end
  /// of
  /// this state's region.
  bool hasRetainForValue(SILValue V, SILInstruction *Retain) const {
    auto Iter = Retains.find(V);
    if (Iter == Retains.end())
      return false;
    return Iter->second.count(Retain);
  }

  /// Returns true if \p Release is a decrement associated with \p V at the
  /// beginning of this state's region.
  bool hasReleaseForValue(SILValue V, SILInstruction *Release) const {
    auto Iter = Releases.find(V);
    if (Iter == Releases.end())
      return false;
    return Iter->second.count(Release);
  }

  const LoopRegion *getRegion() const { return R; }
  const decltype(Retains) &getRetains() const { return Retains; }

  const decltype(Releases) &getReleases() const { return Releases; }

private:
  void performTopDownBlockDataflow(LHContext &State);
  void performTopDownLoopDataflow(LHContext &State);
  void performBottomUpBlockDataflow(LHContext &State);
  void performBottomUpLoopDataflow(LHContext &State);
  void performTopDownMerge(LHContext &State);
  void performBottomUpMerge(LHContext &State);
};

/// A class containing the global state that we need for our computations. This
/// is separate from LoopHoister so that we can pass it into
/// LHRegionState where it is needed.
struct LHContext {
  /// The allocator that we use to allocate our data structures.
  llvm::BumpPtrAllocator Allocator;

  /// A map from the ID of a region to the state that we are tracking for it.
  std::vector<LHRegionState *> RegionIDToStateMap;

  /// The loop region function info that we are using to determine loop
  /// dataflow.
  LoopRegionFunctionInfo *LRFI;

  /// The rc identity analysis we use to pair retains, releases.
  RCIdentityFunctionInfo *RCFI;

  /// A set of retain/releases that are disqualified from being hoisted since
  /// they are reachable or can be reached from a retain/release set that we are
  /// trying to hoist.
  ///
  /// TODO: Be clearer here. Conceptually what I am trying to say is that if we
  /// have a strong_retain and then we see another strong_retain along one path
  /// (so forget the first), but the first strong_retain along another path
  /// reaches the loop header, we can not hoist the first retain since if we
  /// did, we would be removing a retain along a path. A similar thing can
  /// happen with releases.
  llvm::SmallPtrSet<SILInstruction *, 4> UnhoistableRetains;
  llvm::SmallPtrSet<SILInstruction *, 4> UnhoistableReleases;

  LHContext(LoopRegionFunctionInfo *LRFI, RCIdentityFunctionInfo *RCFI);
  ~LHContext();
};

class LoopHoister : public SILLoopVisitor {
  /// All of the state needed for our computation.
  LHContext Ctx;

  /// Did we make a change while hoisting.
  bool MadeChange = false;

public:
  LoopHoister(SILFunction *F, SILLoopInfo *LI, RCIdentityFunctionInfo *RCFI,
              LoopRegionFunctionInfo *LRFI)
      : SILLoopVisitor(F, LI), Ctx{LRFI, RCFI} {}

  ~LoopHoister() = default;

  void runOnFunction(SILFunction *F) override;
  void runOnLoop(SILLoop *L) override;
  void runOnLoopRegion(LoopRegion *R);
  bool madeChange() const { return MadeChange; }

private:
  void performTopDownDataflow(LoopRegion *R);
  void performBottomUpDataflow(LoopRegion *R);
  bool pairIncrementsDecrements(LoopRegion *R);
  void summarizeLoop(LoopRegion *R);
};

} // end swift namespace

#endif
