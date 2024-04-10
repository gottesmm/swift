//===--- PartitionUtils.h -------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_PARTITIONUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_PARTITIONUTILS_H

#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/ImmutablePointerSet.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

#include <algorithm>
#include <variant>

#define DEBUG_TYPE "transfer-non-sendable"

namespace swift {

namespace PartitionPrimitives {

#ifndef NDEBUG
extern bool REGIONBASEDISOLATION_ENABLE_VERBOSE_LOGGING;
#define REGIONBASEDISOLATION_VERBOSE_LOG(...)                                  \
  do {                                                                         \
    if (PartitionPrimitives::REGIONBASEDISOLATION_ENABLE_VERBOSE_LOGGING) {    \
      LLVM_DEBUG(__VA_ARGS__);                                                 \
    }                                                                          \
  } while (0);
#else
#define REGIONBASEDISOLATION_VERBOSE_LOG(...)
#endif

struct Element {
  unsigned num;

  explicit Element(int num) : num(num) {}

  bool operator==(const Element &other) const { return num == other.num; }
  bool operator<(const Element &other) const { return num < other.num; }

  operator unsigned() const { return num; }
};

struct Region {
  unsigned num;

  explicit Region(unsigned num) : num(num) {}

  bool operator==(const Region &other) const { return num == other.num; }
  bool operator<(const Region &other) const { return num < other.num; }

  operator unsigned() const { return num; }
};

} // namespace PartitionPrimitives

} // namespace swift

namespace llvm {

template <>
struct DenseMapInfo<swift::PartitionPrimitives::Region> {
  using Region = swift::PartitionPrimitives::Region;

  static Region getEmptyKey() {
    return Region(DenseMapInfo<unsigned>::getEmptyKey());
  }
  static Region getTombstoneKey() {
    return Region(DenseMapInfo<unsigned>::getTombstoneKey());
  }

  static unsigned getHashValue(Region region) {
    return DenseMapInfo<unsigned>::getHashValue(region);
  }
  static bool isEqual(Region LHS, Region RHS) { return LHS == RHS; }
};

} // namespace llvm

namespace swift {

/// PartitionOpKind represents the different kinds of PartitionOps that
/// SILInstructions can be translated to
enum class PartitionOpKind : uint8_t {
  /// Assign one value to the region of another, takes two args, second arg
  /// must already be tracked with a non-transferred region
  Assign,

  /// Assign one value to a fresh region, takes one arg.
  AssignFresh,

  /// Merge the regions of two values, takes two args, both must be from
  /// non-transferred regions.
  Merge,

  /// Transfer the region of a value if not already transferred, takes one arg.
  Transfer,

  /// Due to an async let or something like that a value that was transferred is
  /// no longer transferred.
  UndoTransfer,

  /// Require the region of a value to be non-transferred, takes one arg.
  Require,
};

} // namespace swift

namespace swift {

class SILIsolationInfo {
public:
  /// The lattice is:
  ///
  /// Unknown -> Disconnected -> TransferringParameter -> Task -> Actor.
  ///
  /// Unknown means no information. We error when merging on it.
  enum Kind {
    Unknown,
    Disconnected,
    Task,
    Actor,
  };

private:
  Kind kind;

  /// The actor isolation if this value has one. The default unspecified case
  /// otherwise.
  ActorIsolation actorIsolation;

  /// This is the value that we got isolation from if we were able to find
  /// one. Used for isolation history.
  SILValue isolatedValue;

  /// If set this is the SILValue that represents the actor instance that we
  /// derived isolatedValue from.
  SILValue actorInstance;

  SILIsolationInfo(ActorIsolation actorIsolation, SILValue isolatedValue,
                   SILValue actorInstance)
      : kind(Actor), actorIsolation(actorIsolation),
        isolatedValue(isolatedValue), actorInstance(actorInstance) {
    assert((!actorInstance ||
            (actorIsolation.getKind() == ActorIsolation::ActorInstance &&
             actorInstance->getType().isAnyActor())) &&
           "actorInstance must be an actor if it is non-empty");
  }

  SILIsolationInfo(Kind kind, SILValue isolatedValue)
      : kind(kind), actorIsolation(), isolatedValue(isolatedValue) {}

  SILIsolationInfo(Kind kind) : kind(kind), actorIsolation() {}

public:
  SILIsolationInfo() : kind(Kind::Unknown), actorIsolation() {}

  operator bool() const { return kind != Kind::Unknown; }

  operator Kind() const { return kind; }

  Kind getKind() const { return kind; }

  bool isDisconnected() const { return kind == Kind::Disconnected; }
  bool isActorIsolated() const { return kind == Kind::Actor; }
  bool isTaskIsolated() const { return kind == Kind::Task; }

  void print(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMP {
    print(llvm::dbgs());
    llvm::dbgs() << '\n';
  }

  void printForDiagnostics(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMPER(dumpForDiagnostics()) {
    printForDiagnostics(llvm::dbgs());
    llvm::dbgs() << '\n';
  }

  ActorIsolation getActorIsolation() const {
    assert(kind == Actor);
    return actorIsolation;
  }

  /// If we are actor or task isolated and could find a specific value that
  /// caused the isolation, put it here. Used for isolation history.
  SILValue getIsolatedValue() const {
    assert(kind == Task || kind == Actor);
    return isolatedValue;
  }

  /// Return the specific SILValue for the actor that our isolated value is
  /// isolated to if one exists.
  SILValue getActorInstance() const {
    assert(kind == Actor);
    return actorInstance;
  }

  bool hasActorIsolation() const { return kind == Actor; }

  bool hasIsolatedValue() const {
    return (kind == Task || kind == Actor) && bool(isolatedValue);
  }

  [[nodiscard]] SILIsolationInfo merge(SILIsolationInfo other) const;

  SILIsolationInfo withActorIsolated(SILValue isolatedValue,
                                     SILValue actorInstance,
                                     ActorIsolation isolation) {
    return SILIsolationInfo::getActorIsolated(isolatedValue, actorInstance,
                                              isolation);
  }

  static SILIsolationInfo getDisconnected() { return {Kind::Disconnected}; }

  static SILIsolationInfo getActorIsolated(SILValue isolatedValue,
                                           SILValue actorInstance,
                                           ActorIsolation actorIsolation) {
    return {actorIsolation, isolatedValue, actorInstance};
  }

  static SILIsolationInfo getActorIsolated(SILValue isolatedValue,
                                           SILValue actorInstance,
                                           NominalTypeDecl *typeDecl) {
    if (typeDecl->isAnyActor())
      return {ActorIsolation::forActorInstanceSelf(typeDecl), isolatedValue,
              actorInstance};
    auto isolation = swift::getActorIsolation(typeDecl);
    if (isolation.isGlobalActor())
      return {isolation, isolatedValue, actorInstance};
    return {};
  }

  static SILIsolationInfo getGlobalActorIsolated(SILValue value,
                                                 Type globalActorType) {
    return getActorIsolated(value, SILValue() /*no actor instance*/,
                            ActorIsolation::forGlobalActor(globalActorType));
  }

  static SILIsolationInfo getTaskIsolated(SILValue value) {
    return {Kind::Task, value};
  }

  /// Attempt to infer the isolation region info for \p inst.
  static SILIsolationInfo get(SILInstruction *inst);

  /// Attempt to infer the isolation region info for \p arg.
  static SILIsolationInfo get(SILArgument *arg);

  static SILIsolationInfo get(SILValue value) {
    if (auto *arg = dyn_cast<SILArgument>(value))
      return get(arg);
    if (auto *inst = dyn_cast<SingleValueInstruction>(value))
      return get(inst);
    return {};
  }

  bool hasSameIsolation(ActorIsolation actorIsolation) const;

  /// Returns true if \p this and \p other have the same isolation. It allows
  /// for the isolated values if any to not match.
  ///
  /// This is useful if one has two non-Sendable values projected from the same
  /// actor or global actor isolated value. E.x.: two different ref_element_addr
  /// from the same actor.
  bool hasSameIsolation(const SILIsolationInfo &other) const;

  /// Returns true if this SILIsolationInfo is deeply equal to other. This means
  /// that the isolation and the isolated value match.
  bool isEqual(const SILIsolationInfo &other) const;

  void Profile(llvm::FoldingSetNodeID &id) const;
};

class Partition;
class TransferringOperandToStateMap;

/// A persistent data structure that is used to "rewind" partition history so
/// that we can discover when values become part of the same region.
///
/// NOTE: This does not track whether or not values are transferred. This is
/// because from the perspective of determining when two values become part of
/// the same region, that information is not important. To unroll history, a
/// Partition must have no transfers to use this. NOTE: There is a method that
/// takes a Partition and produces a new Partition that does not have any
/// transfers.
class IsolationHistory {
public:
  class Factory;

private:
  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  class Node;

  // TODO: This shouldn't need to be a friend.
  friend class Partition;
  friend TransferringOperandToStateMap;
  friend class PartitionStack;

  /// First node in the immutable linked list.
  Node *head = nullptr;
  Factory *factory = nullptr;

  IsolationHistory(Factory *factory) : head(nullptr), factory(factory) {}

public:
  IsolationHistory(const IsolationHistory &otherIsolation)
      : head(otherIsolation.head), factory(otherIsolation.factory) {}

  IsolationHistory &operator=(const IsolationHistory &otherIsolation) {
    assert(factory == otherIsolation.factory);
    head = otherIsolation.head;
    return *this;
  }

  Node *getHead() const { return head; }

  class SequenceBoundarySemantics {
  public:
    enum Kind : std::underlying_type<PartitionOpKind>::type {
      /// Assign one value to the region of another, takes two args, second arg
      /// must already be tracked with a non-transferred region
      Assign,

      /// Assign one value to a fresh region, takes one arg.
      AssignFresh,

      /// Merge the regions of two values, takes two args, both must be from
      /// non-transferred regions.
      Merge,

      /// A single region created from scratch with multiple elements.
      SingleRegion,

      /// Create separate regions for a list of indices.
      SeperateRegions,

      /// A CFG join operation.
      CFGJoin,
    };

  private:
    Kind kind;

  public:
    SequenceBoundarySemantics(Kind kind) : kind(kind) {}

    static std::optional<SequenceBoundarySemantics> get(PartitionOpKind kind) {
      switch (kind) {
      case PartitionOpKind::Assign:
        return Kind::Assign;
      case PartitionOpKind::AssignFresh:
        return Kind::AssignFresh;
      case PartitionOpKind::Merge:
        return Kind::Merge;
      case PartitionOpKind::Transfer:
      case PartitionOpKind::UndoTransfer:
      case PartitionOpKind::Require:
        return {};
      }
    }

    operator Kind() const { return kind; }

    void print(llvm::raw_ostream &os) const;
    SWIFT_DEBUG_DUMP {
      print(llvm::dbgs());
      llvm::dbgs() << '\n';
    }
  };

  /// Push a node that signals the end of a new sequence of history nodes that
  /// should execute together. Must be explicitly ended by a push sequence
  /// end. Is non-rentrant, so one cannot have multiple sequence starts.
  ///
  /// \p loc the SILLocation that identifies the instruction that the "package"
  /// of history nodes that this sequence boundary ends is associated with.
  Node *pushHistorySequenceBoundary(SequenceBoundarySemantics semantics,
                                    SILInstruction *boundaryInst);

  /// Push onto the history list that \p value should be added into its own
  /// independent region.
  Node *pushNewElementRegion(Element element);

  /// Push onto the history that \p value should be removed from a region and
  /// that element is the last element in that region (so the region is empty
  /// afterwards).
  void pushRemoveLastElementFromRegion(Element element);

  /// Push onto the history that \p element should be removed from a region that
  /// contains \p otherElementInOldRegion.
  void pushRemoveElementFromRegion(Element otherElementInOldRegion,
                                   Element element);

  /// \p elementToMergeInto is the element whose region we merge \p otherRegions
  /// into.
  void pushMergeElementRegions(Element elementToMergeInto,
                               ArrayRef<Element> otherRegions);

  /// Assign \p elementToMerge's region to \p elementToMergeInto's region.
  void pushAssignElementRegions(Element elementToMergeInto,
                                Element elementToMerge);

  /// Push that \p other should be merged into this region.
  ///
  /// \arg predBlock the block that we are merging state from.
  /// \arg succBlock the block that we are merging state into.
  void pushCFGHistoryJoin(Node *otherNode, SILBasicBlock *predBlock,
                          SILBasicBlock *succBlock);

  /// Push the top node of \p history as a CFG history join.
  void pushCFGHistoryJoin(IsolationHistory history, SILBasicBlock *predBlock,
                          SILBasicBlock *succBlock) {
    return pushCFGHistoryJoin(history.getHead(), predBlock, succBlock);
  }

  Node *pop();

  bool operator==(const IsolationHistory &other) const {
    return getHead() == other.getHead();
  }

  bool operator!=(const IsolationHistory &other) const {
    return !(*this == other);
  }
};

class IsolationHistory::Node final
    : private llvm::TrailingObjects<IsolationHistory::Node, Element> {
  friend IsolationHistory;
  friend TrailingObjects;

public:
  enum Kind {
    /// Add a new element to its own region. The region will only consist of
    /// element.
    AddNewRegionForElement,

    /// Remove an element from a region which it is the only element of.
    RemoveLastElementFromRegion,

    /// Remove an element from a region which still has elements remaining.
    ///
    /// This is different from RemoveLastElementFromRegion since we store the
    /// other element.
    RemoveElementFromRegion,

    /// Given two elements, data and otherData, merge otherData into data's
    /// region.
    MergeElementRegions,

    /// At a CFG merge point, we merged two histories. We need to visit it
    /// recursively.
    CFGHistoryJoin,

    /// Signals that a sequence boundary has been found in the history and if we
    /// are processing a sequence, should stop processing.
    ///
    /// Clients may want to ensure that a set of history elements are pushed or
    /// popped together since the effects happen at the same time.
    /// HistorySequenceStart
    /// signifies that.
    SequenceBoundary,
  };

private:
  Kind kind;
  Node *parent;

  /// Child node. Never set on construction.
  Node *child = nullptr;

  struct SequenceBoundaryState {
    SequenceBoundarySemantics semantics;
    SILInstruction *boundaryInst;

    SequenceBoundaryState(SequenceBoundarySemantics semantics,
                          SILInstruction *boundaryInst)
        : semantics(semantics), boundaryInst(boundaryInst) {}

    SILLocation getLoc() const {
      if (boundaryInst)
        return boundaryInst->getLoc();
      return SILLocation::invalid();
    }
  };

  struct CFGJoinState {
    Node *node;
    SILBasicBlock *predBlock;
    SILBasicBlock *succBlock;

    CFGJoinState(Node *node, SILBasicBlock *predBlock, SILBasicBlock *succBlock)
        : node(node), predBlock(predBlock), succBlock(succBlock) {}
  };

  /// Contains:
  ///
  /// 1. Node * if we have a CFGHistoryJoin.
  /// 2. A SILLocation if we have a SequenceBoundary.
  /// 3. An element otherwise.
  std::variant<Element, Node *, CFGJoinState, SequenceBoundaryState> subject;

  /// Number of additional element arguments stored in the tail allocated array.
  unsigned numAdditionalElements;

  Node(Kind kind, Node *parent)
      : kind(kind), parent(parent), subject(nullptr) {}
  Node(Kind kind, Node *parent, SequenceBoundarySemantics semantics,
       SILInstruction *boundaryInst)
      : kind(kind), parent(parent),
        subject(SequenceBoundaryState(semantics, boundaryInst)) {}
  Node(Kind kind, Node *parent, Element value)
      : kind(kind), parent(parent), subject(value), numAdditionalElements(0) {}
  Node(Kind kind, Node *parent, Element primaryElement,
       std::initializer_list<Element> restOfTheElements)
      : kind(kind), parent(parent), subject(primaryElement),
        numAdditionalElements(restOfTheElements.size()) {
    unsigned writeIndex = 0;
    for (Element restElt : restOfTheElements) {
      if (primaryElement == restElt) {
        continue;
      }

      getAdditionalElementArgs()[writeIndex] = restElt;
      ++writeIndex;
    }

    // Set writeIndex to n - 1.
    numAdditionalElements = writeIndex;
  }

  Node(Kind kind, Node *parent, Element lhsValue, ArrayRef<Element> rhsValue)
      : kind(kind), parent(parent), subject(lhsValue),
        numAdditionalElements(rhsValue.size()) {
    std::uninitialized_copy(rhsValue.begin(), rhsValue.end(),
                            getAdditionalElementArgs().data());
  }

  Node(Kind kind, Node *parent, Node *node, SILBasicBlock *predBlock,
       SILBasicBlock *succBlock)
      : kind(kind), parent(parent),
        subject(CFGJoinState(node, predBlock, succBlock)),
        numAdditionalElements(0) {}

public:
  Kind getKind() const { return kind; }

  Node *getParent() const { return parent; }

  Node *getChild() const { return child; }
  void setChild(Node *newChild) { child = newChild; }

  Element getFirstArgAsElement() const {
    assert(kind != CFGHistoryJoin);
    assert(std::holds_alternative<Element>(subject));
    return std::get<Element>(subject);
  }

  Node *getFirstArgAsNode() const {
    assert(kind == CFGHistoryJoin);
    assert(std::holds_alternative<CFGJoinState>(subject));
    return std::get<CFGJoinState>(subject).node;
  }

  std::pair<SILBasicBlock *, SILBasicBlock *> getFirstArgAsBlockPair() const {
    assert(kind == CFGHistoryJoin);
    assert(std::holds_alternative<CFGJoinState>(subject));
    auto &state = std::get<CFGJoinState>(subject);
    return {state.predBlock, state.succBlock};
  }

  /// Access the tail allocated buffer of additional element arguments.
  MutableArrayRef<Element> getAdditionalElementArgs() {
    assert(kind == MergeElementRegions || kind == RemoveElementFromRegion);
    return {getTrailingObjects<Element>(), numAdditionalElements};
  }

  ArrayRef<Element> getAdditionalElementArgs() const {
    assert(kind == MergeElementRegions || kind == RemoveElementFromRegion);
    return const_cast<Node *>(this)->getAdditionalElementArgs();
  }

  bool isHistorySequenceBoundary() const {
    return getKind() == SequenceBoundary;
  }

  /// If this node is a history sequence join, return its node. Otherwise,
  /// return nullptr.
  Node *getHistorySequenceJoin() const {
    if (kind != CFGHistoryJoin)
      return nullptr;
    return getFirstArgAsNode();
  }

  std::optional<std::pair<SILBasicBlock *, SILBasicBlock *>>
  getHistorySequenceBlockPair() const {
    if (kind != CFGHistoryJoin)
      return {};
    return getFirstArgAsBlockPair();
  }

  std::optional<SILLocation> getHistoryBoundaryLoc() const {
    if (kind != SequenceBoundary)
      return {};
    return std::get<SequenceBoundaryState>(subject).getLoc();
  }

  SILInstruction *getHistoryBoundaryInst() const {
    if (kind != SequenceBoundary)
      return nullptr;
    return std::get<SequenceBoundaryState>(subject).boundaryInst;
  }

  std::optional<SequenceBoundarySemantics>
  getHistoryBoundaryPartitionOpKind() const {
    if (kind != SequenceBoundary)
      return {};
    return std::get<SequenceBoundaryState>(subject).semantics;
  }

  void print(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMP {
    print(llvm::dbgs());
    llvm::dbgs() << '\n';
  }

  /// Print the isolation history starting at this node and looking through CFG
  /// history joins.
  void printRecursive(llvm::raw_ostream &os,
                      llvm::SmallPtrSetImpl<const Node *> &visitedNodes) const;

  SWIFT_DEBUG_DUMPER(dumpRecursive()) {
    llvm::SmallPtrSet<const Node *, 8> visitedNodes;
    printRecursive(llvm::dbgs(), visitedNodes);
    llvm::dbgs() << '\n';
  }
};

class IsolationHistory::Factory {
  friend IsolationHistory;
  using Node = IsolationHistory::Node;

  llvm::BumpPtrAllocator &allocator;

public:
  Factory(llvm::BumpPtrAllocator &allocator) : allocator(allocator) {}

  Factory(IsolationHistory::Factory &&other) = delete;
  Factory &operator=(IsolationHistory::Factory &&other) = delete;
  Factory(const IsolationHistory::Factory &other) = delete;
  Factory &operator=(const IsolationHistory::Factory &other) = delete;

  /// Returns a new isolation history without any history.
  IsolationHistory get() { return IsolationHistory(this); }
};

struct TransferringOperandState {
  /// The dynamic isolation info of the region of value when we transferred.
  ///
  /// This will contain the isolated value if we found one.
  SILIsolationInfo isolationInfo;

  /// The dynamic isolation history at this point.
  IsolationHistory isolationHistory;

  /// Set to true if the element associated with the operand's vlaue is closure
  /// captured by the user. In such a case, if our element is a sendable var of
  /// a non-Sendable type, we cannot access it since we could race against an
  /// assignment to the var in a closure.
  bool isClosureCaptured;

  TransferringOperandState(IsolationHistory history)
      : isolationInfo(), isolationHistory(history), isClosureCaptured(false) {}
};

class TransferringOperandToStateMap {
  llvm::SmallDenseMap<Operand *, TransferringOperandState> internalMap;
  IsolationHistory::Factory &isolationHistoryFactory;

public:
  TransferringOperandToStateMap(
      IsolationHistory::Factory &isolationHistoryFactory)
      : isolationHistoryFactory(isolationHistoryFactory) {}
  TransferringOperandState &get(Operand *op) const {
    auto *self = const_cast<TransferringOperandToStateMap *>(this);
    auto history = IsolationHistory(&isolationHistoryFactory);
    return self->internalMap.try_emplace(op, TransferringOperandState(history))
        .first->getSecond();
  }
};

} // namespace swift

namespace swift {

/// PartitionOp represents a primitive operation that can be performed on
/// Partitions. This is part of the TransferNonSendable SIL pass workflow:
/// first SILBasicBlocks are compiled to vectors of PartitionOps, then a fixed
/// point partition is found over the CFG.
class PartitionOp {
  using Element = PartitionPrimitives::Element;

private:
  PartitionOpKind opKind;
  llvm::SmallVector<Element, 2> opArgs;

  /// Record the SILInstruction that this PartitionOp was generated from, if
  /// generated during compilation from a SILBasicBlock
  PointerUnion<SILInstruction *, Operand *> source;

  // TODO: can the following declarations be merged?
  PartitionOp(PartitionOpKind opKind, Element arg1,
              SILInstruction *sourceInst = nullptr)
      : opKind(opKind), opArgs({arg1}), source(sourceInst) {
    assert(((opKind != PartitionOpKind::Transfer &&
             opKind != PartitionOpKind::UndoTransfer) ||
            sourceInst) &&
           "Transfer needs a sourceInst");
  }

  PartitionOp(PartitionOpKind opKind, Element arg1, Operand *sourceOperand)
      : opKind(opKind), opArgs({arg1}), source(sourceOperand) {
    assert(((opKind != PartitionOpKind::Transfer &&
             opKind != PartitionOpKind::UndoTransfer) ||
            bool(sourceOperand)) &&
           "Transfer needs a sourceInst");
  }

  PartitionOp(PartitionOpKind opKind, Element arg1, Element arg2,
              SILInstruction *sourceInst = nullptr)
      : opKind(opKind), opArgs({arg1, arg2}), source(sourceInst) {
    assert(((opKind != PartitionOpKind::Transfer &&
             opKind != PartitionOpKind::UndoTransfer) ||
            sourceInst) &&
           "Transfer needs a sourceInst");
  }

  friend class Partition;

public:
  static PartitionOp Assign(Element tgt, Element src,
                            SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Assign, tgt, src, sourceInst);
  }

  static PartitionOp AssignFresh(Element tgt,
                                 SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::AssignFresh, tgt, sourceInst);
  }

  static PartitionOp Transfer(Element tgt, Operand *transferringOp) {
    return PartitionOp(PartitionOpKind::Transfer, tgt, transferringOp);
  }

  static PartitionOp UndoTransfer(Element tgt,
                                  SILInstruction *untransferringInst) {
    return PartitionOp(PartitionOpKind::UndoTransfer, tgt, untransferringInst);
  }

  static PartitionOp Merge(Element tgt1, Element tgt2,
                           SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Merge, tgt1, tgt2, sourceInst);
  }

  static PartitionOp Require(Element tgt,
                             SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Require, tgt, sourceInst);
  }

  bool operator==(const PartitionOp &other) const {
    return opKind == other.opKind && opArgs == other.opArgs &&
           source == other.source;
  };

  bool operator<(const PartitionOp &other) const {
    if (opKind != other.opKind)
      return opKind < other.opKind;
    if (opArgs != other.opArgs)
      return opArgs < other.opArgs;
    return source < other.source;
  }

  PartitionOpKind getKind() const { return opKind; }

  ArrayRef<Element> getOpArgs() const { return opArgs; }

  SILInstruction *getSourceInst() const {
    if (source.is<Operand *>())
      return source.get<Operand *>()->getUser();
    return source.get<SILInstruction *>();
  }

  bool hasSourceInst() const { return source.is<SILInstruction *>(); }

  Operand *getSourceOp() const { return source.get<Operand *>(); }

  SILLocation getSourceLoc() const { return getSourceInst()->getLoc(); }

  void print(llvm::raw_ostream &os, bool extraSpace = false) const;

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }
};

/// A data structure that enables history manipulation of a Partition.
class PartitionStack;

/// A map from Element -> Region that represents the current partition set.
class Partition {
public:
  /// A class defined in PartitionUtils unittest used to grab state from
  /// Partition without exposing it to other users.
  struct PartitionTester;
  friend PartitionStack;

  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  using TransferringOperandSet = ImmutablePointerSet<Operand *>;
  using TransferringOperandSetFactory = ImmutablePointerSetFactory<Operand *>;
  using IsolationHistoryNode = IsolationHistory::Node;

private:
  /// A map from a region number to a instruction that consumes it.
  ///
  /// All we care is that we ever track a single SILInstruction for a region
  /// since we are fine with emitting a single error per value and letting the
  /// user recompile. If this is an ask for in the future, we can use a true
  /// multi map here. The implication of this is that when we are performing
  /// dataflow we use a union operation to combine CFG elements and just take
  /// the first instruction that we see.
  llvm::SmallMapVector<Region, TransferringOperandSet *, 2>
      regionToTransferredOpMap;

  /// Label each index with a non-negative (unsigned) label if it is associated
  /// with a valid region.
  std::map<Element, Region> elementToRegionMap;

  /// Track a label that is guaranteed to be strictly larger than all in use,
  /// and therefore safe for use as a fresh label.
  Region freshLabel = Region(0);

  /// An immutable data structure that we use to push/pop isolation history.
  IsolationHistory history;

  /// In a canonical partition, all regions are labelled with the smallest index
  /// of any member. Certain operations like join and equals rely on
  /// canonicality so when it's invalidated this boolean tracks that, and it
  /// must be reestablished by a call to canonicalize().
  bool canonical;

  /// Create a new partition from a different partiton, only reusing its
  /// non-transferring state.
  ///
  /// Only used by removingTransferState to eliminate potentially copying the
  /// SmallDenseMap that contains transferring state.
  Partition(std::map<Element, Region> elementToRegionMap, Region freshLabel,
            IsolationHistory history, bool isCanonical)
      : elementToRegionMap(elementToRegionMap), freshLabel(freshLabel),
        history(history), canonical(isCanonical) {}

public:
  Partition(IsolationHistory history)
      : elementToRegionMap({}), history(history), canonical(true) {}

  /// 1-arg constructor used when canonicality will be immediately invalidated,
  /// so set to false to begin with
  Partition(IsolationHistory history, bool canonical)
      : elementToRegionMap({}), history(history), canonical(canonical) {}

  /// Return a new Partition that has a single region containing the elements of
  /// \p indices.
  static Partition singleRegion(SILLocation loc, ArrayRef<Element> indices,
                                IsolationHistory inputHistory);

  /// Return a new Partition that has each element of \p indices in their own
  /// region.
  static Partition separateRegions(SILLocation loc, ArrayRef<Element> indices,
                                   IsolationHistory inputHistory);

  /// Test two partititons for equality by first putting them in canonical form
  /// then comparing for exact equality.
  ///
  /// Runs in linear time.
  static bool equals(Partition &fst, Partition &snd) {
    fst.canonicalize();
    snd.canonicalize();

    return fst.elementToRegionMap == snd.elementToRegionMap &&
           fst.regionToTransferredOpMap.size() ==
               snd.regionToTransferredOpMap.size() &&
           llvm::all_of(
               fst.regionToTransferredOpMap,
               [&snd](const std::pair<Region, TransferringOperandSet *> &p) {
                 auto sndIter = snd.regionToTransferredOpMap.find(p.first);
                 return sndIter != snd.regionToTransferredOpMap.end() &&
                        sndIter->second == p.second;
               });
  }

  bool isTrackingElement(Element val) const {
    return elementToRegionMap.count(val);
  }

  /// Mark val as transferred.
  void markTransferred(Element val,
                       TransferringOperandSet *transferredOperandSet);

  /// If val was marked as transferred, unmark it as transfer. Returns true if
  /// we found that \p val was transferred. We return false otherwise.
  bool undoTransfer(Element val);

  /// If \p newElt is not being tracked, create a new region for \p newElt. If
  /// \p newElt is already being tracked, remove it from its old region as well.
  ///
  /// \arg updateHistory internal parameter used to determine if we should
  /// update the history. External users shouldn't use this
  void trackNewElement(Element newElt, bool updateHistory = true);

  /// Assigns \p oldElt to the region associated with \p newElt.
  void assignElement(Element oldElt, Element newElt, bool updateHistory = true);

  bool areElementsInSameRegion(Element firstElt, Element secondElt) const {
    return elementToRegionMap.at(firstElt) == elementToRegionMap.at(secondElt);
  }

  Region getRegion(Element elt) const { return elementToRegionMap.at(elt); }

  using iterator = std::map<Element, Region>::iterator;
  iterator begin() { return elementToRegionMap.begin(); }
  iterator end() { return elementToRegionMap.end(); }
  llvm::iterator_range<iterator> range() { return {begin(), end()}; }

  void clearTransferState() { regionToTransferredOpMap.clear(); }

  Partition removingTransferState() const {
    return {elementToRegionMap, freshLabel, history, canonical};
  }

  /// Returns true if this value has any isolation history stored.
  bool hasHistory() const { return bool(history.getHead()); }

  /// Returns the number of nodes of stored history.
  ///
  /// NOTE: Do not use this in real code... only intended to be used in testing
  /// code.
  unsigned historySize() const {
    unsigned count = 0;
    auto *head = history.getHead();
    if (!head)
      return count;
    ++count;

    while ((head = head->getParent()))
      ++count;

    return count;
  }

  /// Return a copy of our isolation history.
  IsolationHistory getIsolationHistory() const { return history; }

  /// Construct the partition corresponding to the union of the two passed
  /// partitions.
  ///
  /// NOTE: snd is passed in as mutable since we may canonicalize snd. We will
  /// not perform any further mutations to snd.
  ///
  /// Runs in quadratic time.
  ///
  /// \arg predBlock the predecessor block we are merging state out of.
  /// \arg succBlock the successor block we are merging state into.
  static Partition join(const Partition &fst, Partition &snd,
                        SILBasicBlock *predBlock, SILBasicBlock *succBlock);

  /// Return a vector of the transferred values in this partition.
  std::vector<Element> getTransferredVals() const {
    // For effeciency, this could return an iterator not a vector.
    std::vector<Element> transferredVals;
    for (auto [i, _] : elementToRegionMap)
      if (isTransferred(i))
        transferredVals.push_back(i);
    return transferredVals;
  }

  /// Return a vector of the non-transferred regions in this partition, each
  /// represented as a vector of values.
  std::vector<std::vector<Element>> getNonTransferredRegions() const {
    // For effeciency, this could return an iterator not a vector.
    std::map<Region, std::vector<Element>> buckets;

    for (auto [i, label] : elementToRegionMap)
      buckets[label].push_back(i);

    std::vector<std::vector<Element>> doubleVec;

    for (auto [_, bucket] : buckets)
      doubleVec.push_back(bucket);

    return doubleVec;
  }

  void dump_labels() const LLVM_ATTRIBUTE_USED {
    llvm::dbgs() << "Partition";
    if (canonical)
      llvm::dbgs() << "(canonical)";
    llvm::dbgs() << "(fresh=" << freshLabel << "){";
    for (const auto &[i, label] : elementToRegionMap)
      llvm::dbgs() << "[" << i << ": " << label << "] ";
    llvm::dbgs() << "}\n";
  }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  void print(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMPER(dumpVerbose()) { printVerbose(llvm::dbgs()); }

  void printVerbose(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMPER(dumpHistory()) { printHistory(llvm::dbgs()); }
  void printHistory(llvm::raw_ostream &os) const;

  /// See docs on \p history.pushHistorySequenceBoundary().
  IsolationHistoryNode *pushHistorySequenceBoundary(
      IsolationHistory::SequenceBoundarySemantics semantics,
      SILInstruction *boundaryInst) {
    return history.pushHistorySequenceBoundary(semantics, boundaryInst);
  }

  bool isTransferred(Element val) const {
    auto iter = elementToRegionMap.find(val);
    if (iter == elementToRegionMap.end())
      return false;
    return regionToTransferredOpMap.count(iter->second);
  }

  /// Return the instruction that transferred \p val's region or nullptr
  /// otherwise.
  TransferringOperandSet *getTransferred(Element val) const {
    auto iter = elementToRegionMap.find(val);
    if (iter == elementToRegionMap.end())
      return nullptr;
    auto iter2 = regionToTransferredOpMap.find(iter->second);
    if (iter2 == regionToTransferredOpMap.end())
      return nullptr;
    auto *set = iter2->second;
    assert(!set->empty());
    return set;
  }

  /// Validate that all regions in the regionToTransferredOpMap exist in the
  /// elementToRegionMap.
  ///
  /// Asserts when NDEBUG is set. Does nothing otherwise.
  void validateRegionToTransferredOpMapRegions() const {
#ifndef NDEBUG
    llvm::SmallSet<Region, 8> regions;
    for (auto [eltNo, regionNo] : elementToRegionMap) {
      regions.insert(regionNo);
    }
    for (auto [regionNo, opSet] : regionToTransferredOpMap) {
      assert(regions.contains(regionNo) && "Region doesn't exist?!");
    }
#endif
  }

  /// Used only in assertions, check that Partitions promised to be canonical
  /// are actually canonical
  bool is_canonical_correct() const;

  /// Merge the regions of two indices while maintaining canonicality. Returns
  /// the final region used.
  ///
  /// This runs in linear time.
  Region merge(Element fst, Element snd, bool updateHistory = true);

private:
  /// A canonical region is defined to have its region number as equal to the
  /// minimum element number of all of its assigned element numbers. This
  /// routine goes through the element -> region map and transforms the
  /// partition state to restore this property.
  ///
  /// This runs in linear time.
  void canonicalize();

  /// Walk the elementToRegionMap updating all elements in the region of \p
  /// targetElement will be changed to now point at \p newRegion.
  void horizontalUpdate(Element targetElement, Region newRegion,
                        SmallVectorImpl<Element> &mergedElements);

  /// Push onto the history list that \p element should be added into its own
  /// independent region.
  IsolationHistoryNode *pushNewElementRegion(Element element) {
    return history.pushNewElementRegion(element);
  }

  /// Push onto the history that \p element should be removed from the region it
  /// belongs to and that \p element is the last element in that region.
  void pushRemoveLastElementFromRegion(Element element) {
    history.pushRemoveLastElementFromRegion(element);
  }

  /// Push onto the history that \p elementToRemove should be removed from the
  /// region which \p elementFromOldRegion belongs to.
  void pushRemoveElementFromRegion(Element elementFromOldRegion,
                                   Element elementToRemove) {
    history.pushRemoveElementFromRegion(elementFromOldRegion, elementToRemove);
  }

  /// Push that \p other should be merged into this region.
  void pushCFGHistoryJoin(IsolationHistory otherHistory,
                          SILBasicBlock *predBlock, SILBasicBlock *succBlock) {
    if (auto *head = otherHistory.head)
      history.pushCFGHistoryJoin(head, predBlock, succBlock);
  }

  /// NOTE: Assumes that \p elementToMergeInto and \p otherRegions are disjoint.
  void pushMergeElementRegions(Element elementToMergeInto,
                               ArrayRef<Element> otherRegions) {
    history.pushMergeElementRegions(elementToMergeInto, otherRegions);
  }

  /// Remove a single element without touching the region to transferring inst
  /// multimap. Assumes that the element is never the last element in a region.
  ///
  /// Just a helper routine.
  void removeElement(Element e) {
    // We added an element to its own region... so we should remove it and it
    // should be the last element in the region.
    bool result = elementToRegionMap.erase(e);
    canonical = false;
    assert(result && "Failed to erase?!");
  }
};

/// A data structure that enables history manipulation of a Partition.
class PartitionStack {
public:
  using Element = PartitionPrimitives::Element;

  /// If we need to perform recursion due to the values we are looking at being
  /// in the region of the affected elements but not the elements themselves, we
  /// store that information here.
  class RecursionResult {
    /// If we need to recurse to emit the entire diagnostic, this is the new
    /// transferredValueElt to use in the recursion.
    std::optional<Element> newSourceElt;

    /// If we need to recurse to emit the entire diagnostic, this is the new
    /// isolatedSourceValueElt to use in the recursion.
    std::optional<Element> newTargetElt;

  public:
    RecursionResult() {}
    RecursionResult(Element newLHS, Element newRHS)
        : newSourceElt(newLHS), newTargetElt(newRHS) {}

    std::optional<std::pair<Element, Element>> getNewSourceTarget() const {
      if (!newSourceElt || !newTargetElt)
        return {};
      return {{*newSourceElt, *newTargetElt}};
    }

    std::optional<Element> getNewTarget() const { return newTargetElt; }
    std::optional<Element> getNewSource() const { return newSourceElt; }

    void setNewSourceElt(Element elt) {
      assert(!newSourceElt.has_value() && "Cannot set twice");
      newSourceElt = elt;
    }

    void setNewTargetElt(Element elt) {
      assert(!newTargetElt.has_value() && "Cannot set twice");
      newTargetElt = elt;
    }
  };

  class DiagnosticResult {
    SILLocation loc;

    /// For diagnostic purposes this is the element we should as the lhs hand
    /// side of our merge.
    ///
    /// This is merged into \p mergeTargetElement's region.
    std::optional<Element> mergeSourceElement;

    /// For diagnostic purposes this is the element we should as the right hand
    /// side of our merge.
    ///
    /// mergeSourceElement is merged into this element's region.
    std::optional<Element> mergeTargetElement;

  public:
    DiagnosticResult() : loc(SILLocation::invalid()) {}

    SILLocation getLoc() const { return loc; }

    void setLoc(SILLocation newLoc) { loc = newLoc; }

    Element getMergeSource() const {
      assert(mergeSourceElement && "Merge source wasn't set?!");
      return *mergeSourceElement;
    }

    void setMergeSource(Element element) {
      assert(!mergeSourceElement && "Can only be set once");
      mergeSourceElement = element;
    }

    Element getMergeTarget() const {
      assert(mergeTargetElement && "Merge target wasn't set?!");
      return *mergeTargetElement;
    }

    void setMergeTarget(Element element) {
      assert(!mergeTargetElement && "Can only be set once");
      mergeTargetElement = element;
    }

    operator bool() const {
      return mergeSourceElement.has_value() && mergeTargetElement.has_value();
    }
  };

private:
  Partition p;
  SmallVector<IsolationHistory, 8> foundJoinedHistories;
  SmallVectorImpl<RecursionResult> &recursionResults;

public:
  PartitionStack(const Partition &partition,
                 SmallVectorImpl<RecursionResult> &recursionResults)
      : p(partition), recursionResults(recursionResults) {}

  /// Pop the stack until \p transferredValueElt and \p isolatedSourceValueElt
  /// are not part of the same region. Place information about that point into
  /// \p result.
  DiagnosticResult compute(Element transferredValueElt,
                           Element isolatedSourceValueElt);

  ArrayRef<IsolationHistory> getFoundJoinedHistories() const {
    return foundJoinedHistories;
  }

  Partition &getPartition() { return p; }

protected:
  bool pop();
  IsolationHistory::Node *popHistoryOnce();
};

/// A data structure that applies a series of PartitionOps to a single Partition
/// that it modifies.
///
/// Callers use CRTP to modify its behavior. Please see the definition below of
/// a "blank" subclass PartitionOpEvaluatorBaseImpl for a description of the
/// methods needing to be implemented by other CRTP subclasses.
template <typename Impl>
struct PartitionOpEvaluator {
private:
  Impl &asImpl() { return *reinterpret_cast<Impl *>(this); }
  const Impl &asImpl() const { return *reinterpret_cast<const Impl *>(this); }

public:
  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  using TransferringOperandSetFactory =
      Partition::TransferringOperandSetFactory;

protected:
  TransferringOperandSetFactory &ptrSetFactory;
  TransferringOperandToStateMap &operandToStateMap;

  Partition &p;

public:
  PartitionOpEvaluator(Partition &p,
                       TransferringOperandSetFactory &ptrSetFactory,
                       TransferringOperandToStateMap &operandToStateMap)
      : ptrSetFactory(ptrSetFactory), operandToStateMap(operandToStateMap),
        p(p) {}

  /// Call shouldEmitVerboseLogging on our CRTP subclass.
  bool shouldEmitVerboseLogging() const {
    return asImpl().shouldEmitVerboseLogging();
  }

  /// Call handleLocalUseAfterTransfer on our CRTP subclass.
  void handleLocalUseAfterTransfer(const PartitionOp &op, Element elt,
                                   Operand *transferringOp) const {
    return asImpl().handleLocalUseAfterTransfer(op, elt, transferringOp);
  }

  /// Call handleTransferNonTransferrable on our CRTP subclass.
  void
  handleTransferNonTransferrable(const PartitionOp &op, Element elt,
                                 SILIsolationInfo isolationRegionInfo) const {
    return asImpl().handleTransferNonTransferrable(op, elt,
                                                   isolationRegionInfo);
  }
  /// Just call our CRTP subclass.
  void
  handleTransferNonTransferrable(const PartitionOp &op, Element elt,
                                 Element otherElement,
                                 SILIsolationInfo isolationRegionInfo) const {
    return asImpl().handleTransferNonTransferrable(op, elt, otherElement,
                                                   isolationRegionInfo);
  }

  /// Return a reference to the partition we are tracking.
  const Partition &getPartition() const { return p; }

  /// Call isActorDerived on our CRTP subclass.
  bool isActorDerived(Element elt) const {
    return asImpl().isActorDerived(elt);
  }

  SILIsolationInfo getIsolationRegionInfo(Element elt) const {
    return asImpl().getIsolationRegionInfo(elt);
  }

  /// Compute the isolation region info for all elements in \p region.
  ///
  /// The bool result is if it is captured by a closure element. That only is
  /// computed if \p sourceOp is non-null.
  std::pair<SILIsolationInfo, bool>
  getIsolationRegionInfo(Region region, Operand *sourceOp) const {
    bool isClosureCapturedElt = false;
    SILIsolationInfo isolationRegionInfo;

    for (const auto &pair : p.range()) {
      if (pair.second == region) {
        isolationRegionInfo =
            isolationRegionInfo.merge(getIsolationRegionInfo(pair.first));
        if (sourceOp)
          isClosureCapturedElt |= isClosureCaptured(pair.first, sourceOp);
      }
    }

    return {isolationRegionInfo, isClosureCapturedElt};
  }

  /// Overload of \p getIsolationRegionInfo without an Operand.
  SILIsolationInfo getIsolationRegionInfo(Region region) const {
    return getIsolationRegionInfo(region, nullptr).first;
  }

  bool isTaskIsolatedDerived(Element elt) const {
    return asImpl().isTaskIsolatedDerived(elt);
  }

  /// Call isClosureCaptured on our CRTP subclass.
  bool isClosureCaptured(Element elt, Operand *op) const {
    return asImpl().isClosureCaptured(elt, op);
  }

  /// Some evaluators pass in mock instructions that one cannot call getLoc()
  /// upon. So to allow for this, provide a routine that our impl can override
  /// if they need to.
  static SILLocation getLoc(SILInstruction *inst) { return Impl::getLoc(inst); }

  /// Some evaluators pass in mock operands that one cannot call getLoc()
  /// upon. So to allow for this, provide a routine that our impl can override
  /// if they need to.
  static SILLocation getLoc(Operand *op) { return Impl::getLoc(op); }

  /// Some evaluators pass in mock operands that one cannot call getUser()
  /// upon. So to allow for this, provide a routine that our impl can override
  /// if they need to.
  static SILIsolationInfo getIsolationInfo(const PartitionOp &partitionOp) {
    return Impl::getIsolationInfo(partitionOp);
  }

  /// Apply \p op to the partition op.
  void apply(const PartitionOp &op) const {
    if (shouldEmitVerboseLogging()) {
      REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "Applying: ";
                                       op.print(llvm::dbgs()));
      REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "    Before: ";
                                       p.print(llvm::dbgs()));
    }
    SWIFT_DEFER {
      if (shouldEmitVerboseLogging()) {
        REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "    After:  ";
                                         p.print(llvm::dbgs()));
      }
      assert(p.is_canonical_correct());
    };

    // If we have a kind that should result in us pushing new values... push a
    // history sequence boundary.
    auto history = p.getIsolationHistory();
    bool pushedHistorySequenceBoundary = false;
    if (auto boundary =
            IsolationHistory::SequenceBoundarySemantics::get(op.getKind())) {
      SILInstruction *inst = op.hasSourceInst() ? op.getSourceInst() : nullptr;
      p.pushHistorySequenceBoundary(*boundary, inst);
      pushedHistorySequenceBoundary = true;
    }
    SWIFT_DEFER {
      // We either pushed a history boundary or if we did not, then our history
      // should be the same.
      assert((pushedHistorySequenceBoundary ||
              p.getIsolationHistory() == history) &&
             "Should not have pushed any isolation history if we did not push "
             "a history sequence?!");
    };

    switch (op.getKind()) {
    case PartitionOpKind::Assign:
      assert(op.getOpArgs().size() == 2 &&
             "Assign PartitionOp should be passed 2 arguments");
      assert(p.isTrackingElement(op.getOpArgs()[1]) &&
             "Assign PartitionOp's source argument should be already tracked");
      // If we are using a region that was transferred as our assignment source
      // value... emit an error.
      if (auto *transferredOperandSet = p.getTransferred(op.getOpArgs()[1])) {
        for (auto transferredOperand : transferredOperandSet->data()) {
          handleLocalUseAfterTransferHelper(op, op.getOpArgs()[1],
                                            transferredOperand);
        }
      }
      p.assignElement(op.getOpArgs()[0], op.getOpArgs()[1]);
      return;
    case PartitionOpKind::AssignFresh:
      assert(op.getOpArgs().size() == 1 &&
             "AssignFresh PartitionOp should be passed 1 argument");

      p.trackNewElement(op.getOpArgs()[0]);
      return;
    case PartitionOpKind::Transfer: {
      // NOTE: We purposely do not check here if a transferred value is already
      // transferred. Callers are expected to put a require for that
      // purpose. This ensures that if we pass the same argument multiple times
      // to the same transferring function as weakly transferred arguments, we
      // do not get an error.
      assert(op.getOpArgs().size() == 1 &&
             "Transfer PartitionOp should be passed 1 argument");
      assert(p.isTrackingElement(op.getOpArgs()[0]) &&
             "Transfer PartitionOp's argument should already be tracked");

      // Otherwise, we need to merge our isolation region info with the
      // isolation region info of everything else in our region. This is the
      // dynamic isolation region info found by the dataflow.
      Element transferredElement = op.getOpArgs()[0];
      Region transferredRegion = p.getRegion(transferredElement);
      bool isClosureCapturedElt = false;
      SILIsolationInfo transferredRegionIsolation;
      std::tie(transferredRegionIsolation, isClosureCapturedElt) =
          getIsolationRegionInfo(transferredRegion, op.getSourceOp());

      // Before we do anything, see if our dynamic isolation kind is the same as
      // the isolation info for our partition op. If they match, this is not a
      // real transfer operation.
      //
      // DISCUSSION: We couldn't not emit this earlier since we needed the
      // dynamic isolation info of our value.
      if (auto calleeIsolationInfo = getIsolationInfo(op)) {
        if (transferredRegionIsolation.hasSameIsolation(calleeIsolationInfo)) {
          return;
        }
      }

      // If we merged anything, we need to handle a transfer
      // non-transferrable. We pass in the dynamic isolation region info of our
      // region.
      if (bool(transferredRegionIsolation) &&
          !transferredRegionIsolation.isDisconnected()) {
        return handleTransferNonTransferrable(op, op.getOpArgs()[0],
                                              transferredRegionIsolation);
      }

      // Mark op.getOpArgs()[0] as transferred.
      TransferringOperandState &state = operandToStateMap.get(op.getSourceOp());
      state.isClosureCaptured |= isClosureCapturedElt;
      state.isolationInfo =
          state.isolationInfo.merge(transferredRegionIsolation);
      assert(state.isolationInfo && "Cannot have unknown");
      auto *ptrSet = ptrSetFactory.get(op.getSourceOp());
      p.markTransferred(op.getOpArgs()[0], ptrSet);
      return;
    }
    case PartitionOpKind::UndoTransfer: {
      assert(op.getOpArgs().size() == 1 &&
             "UndoTransfer PartitionOp should be passed 1 argument");
      assert(p.isTrackingElement(op.getOpArgs()[0]) &&
             "UndoTransfer PartitionOp's argument should already be tracked");

      // Mark op.getOpArgs()[0] as not transferred.
      p.undoTransfer(op.getOpArgs()[0]);
      return;
    }
    case PartitionOpKind::Merge:
      assert(op.getOpArgs().size() == 2 &&
             "Merge PartitionOp should be passed 2 arguments");
      assert(p.isTrackingElement(op.getOpArgs()[0]) &&
             p.isTrackingElement(op.getOpArgs()[1]) &&
             "Merge PartitionOp's arguments should already be tracked");

      // if attempting to merge a transferred region, handle the failure
      if (auto *transferredOperandSet = p.getTransferred(op.getOpArgs()[0])) {
        for (auto transferredOperand : transferredOperandSet->data()) {
          handleLocalUseAfterTransferHelper(op, op.getOpArgs()[0],
                                            transferredOperand);
        }
      }
      if (auto *transferredOperandSet = p.getTransferred(op.getOpArgs()[1])) {
        for (auto transferredOperand : transferredOperandSet->data()) {
          handleLocalUseAfterTransferHelper(op, op.getOpArgs()[1],
                                            transferredOperand);
        }
      }

      p.merge(op.getOpArgs()[0], op.getOpArgs()[1]);
      return;
    case PartitionOpKind::Require:
      assert(op.getOpArgs().size() == 1 &&
             "Require PartitionOp should be passed 1 argument");
      assert(p.isTrackingElement(op.getOpArgs()[0]) &&
             "Require PartitionOp's argument should already be tracked");
      if (auto *transferredOperandSet = p.getTransferred(op.getOpArgs()[0])) {
        for (auto transferredOperand : transferredOperandSet->data()) {
          handleLocalUseAfterTransferHelper(op, op.getOpArgs()[0],
                                            transferredOperand);
        }
      }
      return;
    }

    llvm_unreachable("Covered switch isn't covered?!");
  }

  void apply(std::initializer_list<PartitionOp> ops) {
    for (auto &o : ops)
      apply(o);
  }

  /// Provides a way for subclasses to disable the error squelching
  /// functionality.
  ///
  /// Used by the unittests.
  bool shouldTryToSquelchErrors() const {
    return asImpl().shouldTryToSquelchErrors();
  }

private:
  // Private helper that squelches the error if our transfer instruction and our
  // use have the same isolation.
  void handleLocalUseAfterTransferHelper(const PartitionOp &op, Element elt,
                                         Operand *transferringOp) const {
    if (shouldTryToSquelchErrors()) {
      if (auto isolationInfo = getIsolationInfo(op)) {
        if (isolationInfo.isActorIsolated() &&
            isolationInfo.hasSameIsolation(
                SILIsolationInfo::get(transferringOp->getUser())))
          return;
      }

      // If our instruction does not have any isolation info associated with it,
      // it must be nonisolated. See if our function has a matching isolation to
      // our transferring operand. If so, we can squelch this.
      if (auto functionIsolation =
              transferringOp->getUser()->getFunction()->getActorIsolation()) {
        if (functionIsolation.isActorIsolated() &&
            SILIsolationInfo::get(transferringOp->getUser())
                .hasSameIsolation(functionIsolation))
          return;
      }
    }

    // Ok, we actually need to emit a call to the callback.
    return handleLocalUseAfterTransfer(op, elt, transferringOp);
  }
};

/// A base implementation that can be used to default initialize CRTP
/// subclasses. Only used to implement base functionality for subclass
/// CRTPs. For true basic evaluation, use PartitionOpEvaluatorBasic below.
template <typename Subclass>
struct PartitionOpEvaluatorBaseImpl : PartitionOpEvaluator<Subclass> {
  using Element = PartitionPrimitives::Element;
  using Region = PartitionPrimitives::Region;
  using TransferringOperandSetFactory =
      Partition::TransferringOperandSetFactory;
  using Super = PartitionOpEvaluator<Subclass>;

  PartitionOpEvaluatorBaseImpl(Partition &workingPartition,
                               TransferringOperandSetFactory &ptrSetFactory,
                               TransferringOperandToStateMap &operandToStateMap)
      : Super(workingPartition, ptrSetFactory, operandToStateMap) {}

  /// Should we emit extra verbose logging statements when evaluating
  /// PartitionOps.
  bool shouldEmitVerboseLogging() const { return true; }

  /// A function called if we discover a transferred value was used after it
  /// was transferred.
  ///
  /// The arguments passed to the closure are:
  ///
  /// 1. The PartitionOp that required the element to be alive.
  ///
  /// 2. The element in the PartitionOp that was asked to be alive.
  ///
  /// 3. The operand of the instruction that originally transferred the
  /// region. Can be used to get the immediate value transferred or the
  /// transferring instruction.
  void handleLocalUseAfterTransfer(const PartitionOp &op, Element elt,
                                   Operand *transferringOp) const {}

  /// This is called if we detect a never transferred element that was passed to
  /// a transfer instruction.
  void handleTransferNonTransferrable(const PartitionOp &op, Element elt,
                                      SILIsolationInfo regionInfo) const {}

  void
  handleTransferNonTransferrable(const PartitionOp &op, Element elt,
                                 Element otherElement,
                                 SILIsolationInfo isolationRegionInfo) const {}

  /// This is used to determine if an element is actor derived. If we determine
  /// that a region containing such an element is transferred, we emit an error
  /// since actor regions cannot be transferred.
  bool isActorDerived(Element elt) const { return false; }

  /// This is used to determine if an element is in the same region as a task
  /// isolated value.
  bool isTaskIsolatedDerived(Element elt) const { return false; }

  /// Returns the information about \p elt's isolation that we ascertained from
  /// SIL and the AST.
  SILIsolationInfo getIsolationRegionInfo(Element elt) const {
    return SILIsolationInfo();
  }

  /// Check if the representative value of \p elt is closure captured at \p
  /// op.
  ///
  /// NOTE: We actually just use the user of \p op in our callbacks. The reason
  /// why we do not just pass in that SILInstruction is that then we would need
  /// to access the instruction in the evaluator which creates a problem when
  /// since the operand we pass in is a dummy operand.
  bool isClosureCaptured(Element elt, Operand *op) const { return false; }

  /// By default squelch errors.
  bool shouldTryToSquelchErrors() const { return true; }

  static SILLocation getLoc(SILInstruction *inst) { return inst->getLoc(); }
  static SILLocation getLoc(Operand *op) { return op->getUser()->getLoc(); }
  static SILIsolationInfo getIsolationInfo(const PartitionOp &partitionOp) {
    return SILIsolationInfo::get(partitionOp.getSourceInst());
  }
};

/// A subclass of PartitionOpEvaluatorBaseImpl that doesn't have any special
/// behavior.
struct PartitionOpEvaluatorBasic final
    : PartitionOpEvaluatorBaseImpl<PartitionOpEvaluatorBasic> {
  PartitionOpEvaluatorBasic(Partition &workingPartition,
                            TransferringOperandSetFactory &ptrSetFactory,
                            TransferringOperandToStateMap &operandToStateMap)
      : PartitionOpEvaluatorBaseImpl(workingPartition, ptrSetFactory,
                                     operandToStateMap) {}
};

} // namespace swift

#endif // SWIFT_PARTITIONUTILS_H
