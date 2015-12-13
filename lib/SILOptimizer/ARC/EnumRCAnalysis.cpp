//===--- EnumRCAnalysis.cpp -----------------------------------------------===//
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

#define DEBUG_TYPE "sil-enum-rc-analysis"
#include "EnumRCAnalysis.h"
#include "swift/Basic/BlotMapVector.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/Support/Debug.h"

using namespace swift;

static void createRefCountOpForPayload(SILBuilder &Builder, SILInstruction *I,
                                       EnumElementDecl *EnumDecl,
                                       SILValue DefOfEnum = SILValue()) {
  assert(EnumDecl->hasArgumentType() &&
         "We assume enumdecl has an argument type");

  SILModule &Mod = I->getModule();

  // The enum value is either passed as an extra argument if we are moving an
  // retain that does not refer to the enum typed value - otherwise it is the
  // argument to the refcount instruction.
  SILValue EnumVal = DefOfEnum ? DefOfEnum : I->getOperand(0);

  SILType ArgType = EnumVal.getType().getEnumElementType(EnumDecl, Mod);

  auto *UEDI =
    Builder.createUncheckedEnumData(I->getLoc(), EnumVal, EnumDecl, ArgType);

  SILType UEDITy = UEDI->getType();

  // If our payload is trivial, we do not need to insert any retain or release
  // operations.
  if (UEDITy.isTrivial(Mod))
    return;

  // If we have a retain value...
  if (isa<RetainValueInst>(I)) {
    // And our payload is refcounted, insert a strong_retain onto the
    // payload.
    if (UEDITy.isReferenceCounted(Mod)) {
      Builder.createStrongRetain(I->getLoc(), UEDI);
      return;
    }

    // Otherwise, insert a retain_value on the payload.
    Builder.createRetainValue(I->getLoc(), UEDI);
    return;
  }

  // At this point we know that we must have a release_value and a non-trivial
  // payload.
  assert(isa<ReleaseValueInst>(I) && "If I is not a retain value here, it must "
         "be a release value since enums do not have reference semantics.");

  // If our payload has reference semantics, insert the strong release.
  if (UEDITy.isReferenceCounted(Mod)) {
    Builder.createStrongRelease(I->getLoc(), UEDI);
    return;
  }

  // Otherwise if our payload is non-trivial but lacking reference semantics,
  // insert the release_value.
  Builder.createReleaseValue(I->getLoc(), UEDI);
}

//===----------------------------------------------------------------------===//
//                             Enum Tag Dataflow
//===----------------------------------------------------------------------===//

namespace {

class BBToDataflowStateMap;

using EnumBBCaseList =
    llvm::SmallVector<std::pair<SILBasicBlock *, EnumElementDecl *>, 2>;

/// Class that performs enum tag state dataflow on the given BB.
class BBEnumTagDataflowState
    : public SILInstructionVisitor<BBEnumTagDataflowState, bool> {
  NullablePtr<SILBasicBlock> BB;

  using ValueToCaseSmallBlotMapVectorTy = BlotMapVector<
      SILValue, EnumElementDecl *, llvm::SmallDenseMap<SILValue, unsigned>,
      llvm::SmallVector<std::pair<SILValue, EnumElementDecl *>, 4>>;
  ValueToCaseSmallBlotMapVectorTy ValueToCaseMap;

  using EnumToEnumBBCaseListMapTy =
      BlotMapVector<SILValue, EnumBBCaseList,
                    llvm::SmallDenseMap<SILValue, unsigned>,
                    llvm::SmallVector<std::pair<SILValue, EnumBBCaseList>, 4>>;

  EnumToEnumBBCaseListMapTy EnumToEnumBBCaseListMap;

public:
  BBEnumTagDataflowState() = default;
  BBEnumTagDataflowState(const BBEnumTagDataflowState &Other) = default;
  ~BBEnumTagDataflowState() = default;

  bool init(SILBasicBlock *NewBB) {
    assert(NewBB && "NewBB should not be null");
    BB = NewBB;
    return true;
  }

  SILBasicBlock *getBB() { return BB.get(); }

  using iterator = decltype(ValueToCaseMap)::iterator;
  iterator begin() { return ValueToCaseMap.getItems().begin(); }
  iterator end() { return ValueToCaseMap.getItems().begin(); }
  iterator_range<iterator> currentTrackedState() {
    return ValueToCaseMap.getItems();
  }

  void clear() { ValueToCaseMap.clear(); }

  bool visitValueBase(ValueBase *V) { return false; }

  bool visitEnumInst(EnumInst *EI) {
    DEBUG(llvm::dbgs() << "    Storing enum into map: " << *EI);
    ValueToCaseMap[SILValue(EI)] = EI->getElement();
    return false;
  }

  bool visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI) {
    DEBUG(llvm::dbgs() << "    Storing unchecked enum data into map: "
                       << *UEDI);
    ValueToCaseMap[SILValue(UEDI->getOperand())] = UEDI->getElement();
    return false;
  }

  bool visitRetainValueInst(RetainValueInst *RVI);
  bool visitReleaseValueInst(ReleaseValueInst *RVI);
  bool process();
  bool hoistDecrementsIntoSwitchRegions(AliasAnalysis *AA);
  bool sinkIncrementsOutOfSwitchRegions(AliasAnalysis *AA,
                                        RCIdentityFunctionInfo *RCIA);
  void handlePredSwitchEnum(SwitchEnumInst *S);
  void handlePredCondSelectEnum(CondBranchInst *CondBr);

  /// Helper method which initializes this state map with the data from the
  /// first predecessor BB.
  ///
  /// We will be performing an intersection in a later step of the merging.
  bool initWithFirstPred(BBToDataflowStateMap &BBToStateMap,
                         SILBasicBlock *FirstPredBB);

  /// Top level merging function for predecessors.
  void mergePredecessorStates(BBToDataflowStateMap &BBToStateMap);

  ///
  void mergeSinglePredTermInfoIntoState(BBToDataflowStateMap &BBToStateMap,
                                        SILBasicBlock *Pred);
};

/// Map all blocks to BBEnumTagDataflowState in RPO order.
class BBToDataflowStateMap {
  llvm::DenseMap<SILBasicBlock *, unsigned> BBToRPOMap;
  std::vector<BBEnumTagDataflowState> BBToStateVec;

public:
  BBToDataflowStateMap(PostOrderAnalysis *POTA, SILFunction *F) {
    auto ReversePostOrder = POTA->get(F)->getReversePostOrder();
    int PostOrderSize =
        std::distance(ReversePostOrder.begin(), ReversePostOrder.end());
    BBToStateVec.resize(PostOrderSize);
    unsigned RPOIdx = 0;
    for (SILBasicBlock *BB : ReversePostOrder) {
      BBToStateVec[RPOIdx].init(BB);
      BBToRPOMap[BB] = RPOIdx;
      ++RPOIdx;
    }
  }
  unsigned size() const { return BBToStateVec.size(); }
  BBEnumTagDataflowState &getRPOState(unsigned RPOIdx) {
    return BBToStateVec[RPOIdx];
  }
  /// \return BBEnumTagDataflowState or NULL for unreachable blocks.
  BBEnumTagDataflowState *getBBState(SILBasicBlock *BB) {
    auto Iter = BBToRPOMap.find(BB);
    if (Iter == BBToRPOMap.end())
      return nullptr;
    return &getRPOState(Iter->second);
  }
};

} // end anonymous namespace

void BBEnumTagDataflowState::handlePredSwitchEnum(SwitchEnumInst *S) {

  // Find the tag associated with our BB and set the state of the
  // enum we switch on to that value. This is important so we can determine
  // covering switches for enums that have cases without payload.

  // Next check if we are the target of a default switch_enum case. If we are,
  // no interesting information can be extracted, so bail...
  if (S->hasDefault() && S->getDefaultBB() == getBB())
    return;

  // Otherwise, attempt to find the tag associated with this BB in the switch
  // enum...
  for (unsigned i = 0, e = S->getNumCases(); i != e; ++i) {
    auto P = S->getCase(i);

    // If this case of the switch is not matched up with this BB, skip the
    // case...
    if (P.second != getBB())
      continue;

    // Ok, we found the case for our BB. If we don't have an enum tag (which can
    // happen if we have a default statement), return. There is nothing more we
    // can do.
    if (!P.first)
      return;

    // Ok, we have a matching BB and a matching enum tag. Set the state and
    // return.
    ValueToCaseMap[S->getOperand()] = P.first;
    return;
  }
  llvm_unreachable("A successor of a switch_enum terminated BB should be in "
                   "the switch_enum.");
}

void BBEnumTagDataflowState::handlePredCondSelectEnum(CondBranchInst *CondBr) {

  SelectEnumInst *EITI = dyn_cast<SelectEnumInst>(CondBr->getCondition());
  if (!EITI)
    return;

  NullablePtr<EnumElementDecl> TrueElement = EITI->getSingleTrueElement();
  if (TrueElement.isNull())
    return;

  // Find the tag associated with our BB and set the state of the
  // enum we switch on to that value. This is important so we can determine
  // covering switches for enums that have cases without payload.

  // Check if we are the true case, ie, we know that we are the given tag.
  const auto &Operand = EITI->getEnumOperand();
  if (CondBr->getTrueBB() == getBB()) {
    ValueToCaseMap[Operand] = TrueElement.get();
    return;
  }

  // If the enum only has 2 values and its tag isn't the true branch, then we
  // know the true branch must be the other tag.
  if (EnumDecl *E = Operand.getType().getEnumOrBoundGenericEnum()) {
    // Look for a single other element on this enum.
    EnumElementDecl *OtherElt = nullptr;
    for (EnumElementDecl *Elt : E->getAllElements()) {
      // Skip the case where we find the select_enum element
      if (Elt == TrueElement.get())
        continue;
      // If we find another element, then we must have more than 2, so bail.
      if (OtherElt)
        return;
      OtherElt = Elt;
    }
    // Only a single enum element?  How would this even get here?  We should
    // handle it in SILCombine.
    if (!OtherElt)
      return;
    // FIXME: Can we ever not be the false BB here?
    if (CondBr->getTrueBB() != getBB()) {
      ValueToCaseMap[Operand] = OtherElt;
      return;
    }
  }
}

bool BBEnumTagDataflowState::initWithFirstPred(
    BBToDataflowStateMap &BBToStateMap, SILBasicBlock *FirstPredBB) {
  // Try to look up the state for the first pred BB.
  BBEnumTagDataflowState *FirstPredState = BBToStateMap.getBBState(FirstPredBB);

  // If we fail, we found an unreachable block, bail.
  if (FirstPredState == nullptr) {
    DEBUG(llvm::dbgs() << "        Found an unreachable block!\n");
    return false;
  }

  // Ok, our state is in the map, copy in the predecessors value to case map.
  ValueToCaseMap = FirstPredState->ValueToCaseMap;

  // If we are predecessors only successor, we can potentially hoist releases
  // into it, so associate the first pred BB and the case for each value that we
  // are tracking with it.
  //
  // TODO: I am writing this too fast. Clean this up later.
  if (FirstPredBB->getSingleSuccessor())
    for (auto P : ValueToCaseMap.getItems())
      EnumToEnumBBCaseListMap[P.first].push_back({FirstPredBB, P.second});

  return true;
}

void BBEnumTagDataflowState::mergeSinglePredTermInfoIntoState(
    BBToDataflowStateMap &BBToStateMap, SILBasicBlock *Pred) {
  // Grab the terminator of our one predecessor and if it is a switch enum, mix
  // it into this state.
  TermInst *PredTerm = Pred->getTerminator();
  if (auto *S = dyn_cast<SwitchEnumInst>(PredTerm)) {
    handlePredSwitchEnum(S);
    return;
  }

  auto *CondBr = dyn_cast<CondBranchInst>(PredTerm);
  if (!CondBr)
    return;

  handlePredCondSelectEnum(CondBr);
}

void BBEnumTagDataflowState::mergePredecessorStates(
    BBToDataflowStateMap &BBToStateMap) {

  // If we have no precessors, there is nothing to do so return early...
  if (getBB()->pred_empty()) {
    DEBUG(llvm::dbgs() << "            No Preds.\n");
    return;
  }

  auto PI = getBB()->pred_begin(), PE = getBB()->pred_end();
  if (*PI == getBB()) {
    DEBUG(llvm::dbgs() << "            Found a self loop. Bailing!\n");
    return;
  }

  // Grab the first predecessor BB.
  SILBasicBlock *FirstPred = *PI;
  ++PI;

  // Attempt to initialize our state with our first predecessor's state by just
  // copying. We will be doing an intersection with all of the other BB.
  if (!initWithFirstPred(BBToStateMap, FirstPred))
    return;

  // If we only have one predecessor see if we can gain any information and or
  // knowledge from the terminator of our one predecessor. There is nothing more
  // that we can do, return.
  //
  // This enables us to get enum information from switch_enum and cond_br about
  // the value that an enum can take in our block. This is a common case that
  // comes up.
  if (PI == PE) {
    mergeSinglePredTermInfoIntoState(BBToStateMap, FirstPred);
    return;
  }

  DEBUG(llvm::dbgs() << "            Merging in rest of predecessors...\n");

  // Enum values that while merging we found conflicting values for. We blot
  // them after the loop in order to ensure that we can still find the ends of
  // switch regions.
  llvm::SmallVector<SILValue, 4> CurBBValuesToBlot;

  // If we do not find state for a specific value in any of our predecessor BBs,
  // we can not be the end of a switch region since we can not cover our
  // predecessor BBs with enum decls. Blot after the loop.
  llvm::SmallVector<SILValue, 4> PredBBValuesToBlot;

  // And for each remaining predecessor...
  do {
    // If we loop on ourselves, bail...
    if (*PI == getBB()) {
      DEBUG(llvm::dbgs() << "            Found a self loop. Bailing!\n");
      return;
    }

    // Grab the predecessors state...
    SILBasicBlock *PredBB = *PI;

    BBEnumTagDataflowState *PredBBState = BBToStateMap.getBBState(PredBB);
    if (PredBBState == nullptr) {
      DEBUG(llvm::dbgs() << "            Found an unreachable block!\n");
      return;
    }

    ++PI;

    // Then for each (SILValue, Enum Tag) that we are tracking...
    for (auto P : ValueToCaseMap.getItems()) {
      // If this SILValue was blotted, there is nothing left to do, we found
      // some sort of conflicting definition and are being conservative.
      if (!P.first)
        continue;

      // Then attempt to look up the enum state associated in our SILValue in
      // the predecessor we are processing.
      auto PredValue = PredBBState->ValueToCaseMap.find(P.first);

      // If we can not find the state associated with this SILValue in this
      // predecessor or the value in the corresponding predecessor was blotted,
      // we can not find a covering switch for this BB or forward any enum tag
      // information for this enum value.
      if (PredValue == PredBBState->ValueToCaseMap.end() || !PredValue->first) {
        // Otherwise, we are conservative and do not forward the EnumTag that we
        // are tracking. Blot it!
        DEBUG(llvm::dbgs() << "                Blotting: " << P.first);
        CurBBValuesToBlot.push_back(P.first);
        PredBBValuesToBlot.push_back(P.first);
        continue;
      }

      // Check if out predecessor has any other successors. If that is true we
      // clear all the state since we can not hoist safely.
      if (!PredBB->getSingleSuccessor()) {
        EnumToEnumBBCaseListMap.clear();
        DEBUG(llvm::dbgs() << "                Predecessor has other "
                              "successors. Clearing BB cast list map.\n");
      } else {
        // Otherwise, add this case to our predecessor case list. We will unique
        // this after we have finished processing all predecessors.
        auto Case = std::make_pair(PredBB, PredValue->second);
        EnumToEnumBBCaseListMap[PredValue->first].push_back(Case);
      }

      // And the states match, the enum state propagates to this BB.
      if (PredValue->second == P.second)
        continue;

      // Otherwise, we are conservative and do not forward the EnumTag that we
      // are tracking. Blot it!
      DEBUG(llvm::dbgs() << "                Blotting: " << P.first);
      CurBBValuesToBlot.push_back(P.first);
    }
  } while (PI != PE);

  for (SILValue V : CurBBValuesToBlot) {
    ValueToCaseMap.blot(V);
  }
  for (SILValue V : PredBBValuesToBlot) {
    EnumToEnumBBCaseListMap.blot(V);
  }
}

bool BBEnumTagDataflowState::visitRetainValueInst(RetainValueInst *RVI) {
  auto FindResult = ValueToCaseMap.find(RVI->getOperand());
  if (FindResult == ValueToCaseMap.end())
    return false;

  // If we do not have any argument, kill the retain_value.
  if (!FindResult->second->hasArgumentType()) {
    RVI->eraseFromParent();
    return true;
  }

  DEBUG(llvm::dbgs() << "    Found RetainValue: " << *RVI);
  DEBUG(llvm::dbgs() << "        Paired to Enum Oracle: " << FindResult->first);

  SILBuilderWithScope<> Builder(RVI, RVI->getDebugScope());
  createRefCountOpForPayload(Builder, RVI, FindResult->second);
  RVI->eraseFromParent();
  return true;
}

bool BBEnumTagDataflowState::visitReleaseValueInst(ReleaseValueInst *RVI) {
  auto FindResult = ValueToCaseMap.find(RVI->getOperand());
  if (FindResult == ValueToCaseMap.end())
    return false;

  // If we do not have any argument, just delete the release value.
  if (!FindResult->second->hasArgumentType()) {
    RVI->eraseFromParent();
    return true;
  }

  DEBUG(llvm::dbgs() << "    Found ReleaseValue: " << *RVI);
  DEBUG(llvm::dbgs() << "        Paired to Enum Oracle: " << FindResult->first);

  SILBuilderWithScope<> Builder(RVI, RVI->getDebugScope());
  createRefCountOpForPayload(Builder, RVI, FindResult->second);
  RVI->eraseFromParent();
  return true;
}

bool BBEnumTagDataflowState::process() {
  bool Changed = false;

  auto SI = getBB()->begin();
  while (SI != getBB()->end()) {
    SILInstruction *I = &*SI;
    ++SI;
    Changed |= visit(I);
  }

  return Changed;
}

bool BBEnumTagDataflowState::hoistDecrementsIntoSwitchRegions(
    AliasAnalysis *AA) {
  bool Changed = false;
  unsigned NumPreds = std::distance(getBB()->pred_begin(), getBB()->pred_end());

  for (auto II = getBB()->begin(), IE = getBB()->end(); II != IE;) {
    auto *RVI = dyn_cast<ReleaseValueInst>(&*II);
    ++II;

    // If this instruction is not a release, skip it...
    if (!RVI)
      continue;

    DEBUG(llvm::dbgs() << "        Visiting release: " << *RVI);

    // Grab the operand of the release value inst.
    SILValue Op = RVI->getOperand();

    // Lookup the [(BB, EnumTag)] list for this operand.
    auto R = EnumToEnumBBCaseListMap.find(Op);
    // If we don't have one, skip this release value inst.
    if (R == EnumToEnumBBCaseListMap.end()) {
      DEBUG(llvm::dbgs() << "            Could not find [(BB, EnumTag)] "
                            "list for release_value's operand. Bailing!\n");
      continue;
    }

    auto &EnumBBCaseList = R->second;
    // If we don't have an enum tag for each predecessor of this BB, bail since
    // we do not know how to handle that BB.
    if (EnumBBCaseList.size() != NumPreds) {
      DEBUG(
          llvm::dbgs()
          << "            Found [(BB, EnumTag)] "
             "list for release_value's operand, but we do not have an enum tag "
             "for each predecessor. Bailing!\n");
      DEBUG(llvm::dbgs() << "            List:\n");
      DEBUG(for (auto P
                 : EnumBBCaseList) {
        llvm::dbgs() << "                ";
        P.second->dump(llvm::dbgs());
      });
      continue;
    }

    // Finally ensure that we have no users of this operand preceding the
    // release_value in this BB. If we have users like that we can not hoist the
    // release past them unless we know that there is an additional set of
    // releases that together post-dominate this release. If we can not do this,
    // skip this release.
    //
    // TODO: We need information from the ARC optimizer to prove that property
    // if we are going to use it.
    if (valueHasARCUsesInInstructionRange(Op, getBB()->begin(),
                                          SILBasicBlock::iterator(RVI), AA)) {
      DEBUG(llvm::dbgs() << "            Release value has use that stops "
                            "hoisting! Bailing!\n");
      continue;
    }

    DEBUG(llvm::dbgs() << "            Its safe to perform the "
                          "transformation!\n");

    // Otherwise perform the transformation.
    for (auto P : EnumBBCaseList) {
      // If we don't have an argument for this case, there is nothing to
      // do... continue...
      if (!P.second->hasArgumentType())
        continue;

      // Otherwise create the release_value before the terminator of the
      // predecessor.
      assert(P.first->getSingleSuccessor() &&
             "Can not hoist release into BB that has multiple successors");
      SILBuilderWithScope<> Builder(P.first->getTerminator(),
                                    RVI->getDebugScope());
      createRefCountOpForPayload(Builder, RVI, P.second);
    }

    RVI->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

static SILInstruction *findLastSinkableMatchingEnumValueRCIncrementInPred(
    AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA, SILValue EnumValue,
    SILBasicBlock *BB) {
  // Otherwise, see if we can find a retain_value or strong_retain associated
  // with that enum in the relevant predecessor.
  auto FirstInc = std::find_if(
      BB->rbegin(), BB->rend(),
      [&RCIA, &EnumValue](const SILInstruction &I) -> bool {
        // If I is not an increment, ignore it.
        if (!isa<StrongRetainInst>(I) && !isa<RetainValueInst>(I))
          return false;

        // Otherwise, if the increments operand stripped of RC identity
        // preserving
        // ops matches EnumValue, it is the first increment we are interested
        // in.
        return EnumValue == RCIA->getRCIdentityRoot(I.getOperand(0));
      });

  // If we do not find a ref count increment in the relevant BB, skip this
  // enum since there is nothing we can do.
  if (FirstInc == BB->rend())
    return nullptr;

  // Otherwise, see if there are any instructions in between FirstPredInc and
  // the end of the given basic block that could decrement first pred. If such
  // an instruction exists, we can not perform this optimization so continue.
  if (valueHasARCDecrementOrCheckInInstructionRange(EnumValue, &*FirstInc,
                                                    BB->getTerminator(), AA))
    return nullptr;

  return &*FirstInc;
}

static bool findRetainsSinkableFromSwitchRegionForEnum(
    AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA, SILValue EnumValue,
    EnumBBCaseList &Map, SmallVectorImpl<SILInstruction *> &DeleteList) {

  // For each predecessor with argument type...
  for (auto &P : Map) {
    SILBasicBlock *PredBB = P.first;
    EnumElementDecl *Decl = P.second;

    // If the case does not have an argument type, skip the predecessor since
    // there will not be a retain to sink.
    if (!Decl->hasArgumentType())
      continue;

    // Ok, we found a payloaded predecessor. Look backwards through the
    // predecessor for the first ref count increment on EnumValue. If there
    // are no ref count decrements in between the increment and the terminator
    // of the BB, then we can sink the retain out of the switch enum.
    auto *Inc = findLastSinkableMatchingEnumValueRCIncrementInPred(
        AA, RCIA, EnumValue, PredBB);
    // If we do not find such an increment, there is nothing we can do, bail.
    if (!Inc)
      return false;

    // Otherwise add the increment to the delete list.
    DeleteList.push_back(Inc);
  }

  // If we were able to process each predecessor successfully, return true.
  return true;
}

bool BBEnumTagDataflowState::sinkIncrementsOutOfSwitchRegions(
    AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA) {
  bool Changed = false;
  unsigned NumPreds = std::distance(getBB()->pred_begin(), getBB()->pred_end());
  llvm::SmallVector<SILInstruction *, 4> DeleteList;

  // For each (EnumValue, [(BB, EnumTag)]) that we are tracking...
  for (auto &P : EnumToEnumBBCaseListMap) {
    // Clear our delete list.
    DeleteList.clear();

    // If EnumValue is null, we deleted this entry. There is nothing to do for
    // this value... Skip it.
    if (!P.first)
      continue;
    SILValue EnumValue = RCIA->getRCIdentityRoot(P.first);
    EnumBBCaseList &Map = P.second;

    // If we do not have a tag associated with this enum value for each
    // predecessor, we are not a switch region exit for this enum value. Skip
    // this value.
    if (Map.size() != NumPreds)
      continue;

    // Look through our predecessors for a set of ref count increments on our
    // enum value for every payloaded case that *could* be sunk. If we miss an
    // increment from any of the payloaded case there is nothing we can do here,
    // so skip this enum value.
    if (!findRetainsSinkableFromSwitchRegionForEnum(AA, RCIA, EnumValue, Map,
                                                    DeleteList))
      continue;

    // If we do not have any payload arguments, then we should have an empty
    // delete list and there is nothing to do here.
    if (DeleteList.empty())
      continue;

    // Ok, we can perform this transformation! Insert the new retain_value and
    // delete all of the ref count increments from the predecessor BBs.
    //
    // TODO: Which debug loc should we use here? Using one of the locs from the
    // delete list seems reasonable for now...
    SILBuilder(getBB()->begin())
        .createRetainValue(DeleteList[0]->getLoc(), EnumValue);
    for (auto *I : DeleteList)
      I->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

namespace {

using EnumRCData = std::tuple<SILValue, SILBasicBlock *BB, EnumElementDecl *>;

} // end anonymous namespace

static bool
getEnumRCDataForInstruction(SILInstruction *I,
                            llvm::SmallVectorImpl<EnumRCData> &Data) {

}

//===----------------------------------------------------------------------===//
//                          EnumRCFunctionInfoPImpl
//===----------------------------------------------------------------------===//

class EnumRCFunctionInfo::EnumRCFunctionInfoPImpl : public EnumRCFunctionInfo {
  SILFunction *F;

  PostOrderFunctionInfo *PO;

  llvm::SmallDenseMap<SILValue, unsigned, 4> ValueToIndexMap;
  llvm::SmallVector<llvm::SparseBitVector, 16> Data;

public:
  EnumRCFunctionInfoPImpl(SILFunction *F, PostOrderFunctionInfo *PO);

  bool doesEnumNeedRCAtBB(SILBasicBlock *BB, SILValue V) const {
    auto Iter = ValueToIndexMap.find(V);
    if (Iter == ValueToIndexMap.end())
      return true;

    if (auto ID = PO->getRPONum(BB)) {

      
      auto Iter = ValueToNoRCMap.find(V);
      if (Iter == ValueToNoRCMap.end())
        return true;
      return Iter->second[ID.getValue()];
    }
    return true;
  }
};

EnumRCFunctionInfo::EnumRCFunctionInfoPImpl::
EnumRCFunctionInfoPImpl(SILFunction *F, PostOrderFunctionInfo *PO)
  : F(F), PO(PO), BBToStateVec(PO->size()) {

  // This is a vector we use as a set of values. After we have found all of our
  // target values, we sort/unique the vector turning it into a set which we can
  // use to determine the size of all of the SmallBitVectors we have.
  std::vector<SILValue> ValueSet;
  std::vector<std::tuple<unsigned, SILBasicBlock *, EnumElementDecl *>> Data;

  llvm::SmallVector<EnumRCData, 4> Tmp;

  // Go through each BB and find all of the instructions that *can* act as an
  // oracle with respect to an SSA enum value's case.
  for (auto *BB : PO->getReversePostOrder()) {
    for (auto &I : *BB) {
      assert(Tmp.empty() && "Did not clear tmp after "
             "getEnumRCDataForInstruction succeeds?!");
      if (getEnumRCDataForInstruction(&I, Tmp)) {
        
        Tmp.clear();
      }
    }
  }

  // Then we sort unique/values so we can get a count of the number of items we
  // have.
  sortUnique(Values);
}

//===----------------------------------------------------------------------===//
//                             EnumRCFunctionInfo
//===----------------------------------------------------------------------===//

EnumRCFunctionInfo::~EnumRCFunctionInfo() {
  // We need to call the PImpl's destructor since we do not want to use a
  // virtual destructor and to the outside world, we have an EnumRCFunctionInfo.
  asPImpl().~EnumRCFunctionInfoPImpl();
}

EnumRCFunctionInfo *EnumRCFunctionInfo::create(SILFunction *F,
                                               PostOrderFunctionInfo *PO) {
  return new EnumRCFunctionInfoPImpl(F, PO);
}

bool EnumRCFunctionInfo::doesEnumNeedRCAtBB(SILBasicBlock *BB,
                                            SILValue V) const {
  return asPImpl().doesEnumNeedRCAtBB(BB, V);
}

//===----------------------------------------------------------------------===//
//                              Enum RC Analysis
//===----------------------------------------------------------------------===//

EnumRCFunctionInfo *
EnumRCAnalysis::
newFunctionAnalysis(SILFunction *F) {
  return EnumRCFunctionInfo::create(F, POA->get(F));
}

//===----------------------------------------------------------------------===//
//                           Top Level Entry Points
//===----------------------------------------------------------------------===//

SILAnalysis *swift::createEnumRCAnalysis(SILModule *M, SILPassManager *PM) {
  return new EnumRCAnalysis(PM);
}
