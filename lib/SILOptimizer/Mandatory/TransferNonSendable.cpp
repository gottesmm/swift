//===--- TransferNonSendable.cpp ------------------------------------------===//
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

#define DEBUG_TYPE "transfer-non-sendable"

#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Type.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/ImmutablePointerSet.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/MemoryLocations.h"
#include "swift/SIL/NodeDatastructures.h"
#include "swift/SIL/OperandDatastructures.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/Analysis/RegionAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/PartitionUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::PartitionPrimitives;
using namespace swift::PatternMatch;
using namespace swift::regionanalysisimpl;

namespace {

using TransferringOperandSetFactory = Partition::TransferringOperandSetFactory;
using TrackableValueID = PartitionPrimitives::Element;
using Region = PartitionPrimitives::Region;

} // namespace

//===----------------------------------------------------------------------===//
//                             MARK: Diagnostics
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(ASTContext &context, SourceLoc loc,
                                        Diag<T...> diag, U &&...args) {
  return std::move(context.Diags.diagnose(loc, diag, std::forward<U>(args)...)
                       .warnUntilSwiftVersion(6));
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(ASTContext &context, SILLocation loc,
                                        Diag<T...> diag, U &&...args) {
  return ::diagnoseError(context, loc.getSourceLoc(), diag,
                         std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(const PartitionOp &op, Diag<T...> diag,
                                        U &&...args) {
  return ::diagnoseError(op.getSourceInst()->getFunction()->getASTContext(),
                         op.getSourceLoc().getSourceLoc(), diag,
                         std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(const Operand *op, Diag<T...> diag,
                                        U &&...args) {
  return ::diagnoseError(op->getUser()->getFunction()->getASTContext(),
                         op->getUser()->getLoc().getSourceLoc(), diag,
                         std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(const SILInstruction *inst,
                                        Diag<T...> diag, U &&...args) {
  return ::diagnoseError(inst->getFunction()->getASTContext(),
                         inst->getLoc().getSourceLoc(), diag,
                         std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(ASTContext &context, SourceLoc loc,
                                       Diag<T...> diag, U &&...args) {
  return context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(ASTContext &context, SILLocation loc,
                                       Diag<T...> diag, U &&...args) {
  return ::diagnoseNote(context, loc.getSourceLoc(), diag,
                        std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(const PartitionOp &op, Diag<T...> diag,
                                       U &&...args) {
  return ::diagnoseNote(op.getSourceInst()->getFunction()->getASTContext(),
                        op.getSourceLoc().getSourceLoc(), diag,
                        std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(const Operand *op, Diag<T...> diag,
                                       U &&...args) {
  return ::diagnoseNote(op->getUser()->getFunction()->getASTContext(),
                        op->getUser()->getLoc().getSourceLoc(), diag,
                        std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(const SILInstruction *inst,
                                       Diag<T...> diag, U &&...args) {
  return ::diagnoseNote(inst->getFunction()->getASTContext(),
                        inst->getLoc().getSourceLoc(), diag,
                        std::forward<U>(args)...);
}

//===----------------------------------------------------------------------===//
//                           MARK: Require Liveness
//===----------------------------------------------------------------------===//

namespace {

class BlockLivenessInfo {
  // Generation counter so we do not need to reallocate.
  unsigned generation = 0;
  SILInstruction *firstRequireInst = nullptr;

  void resetIfNew(unsigned newGeneration) {
    if (generation == newGeneration)
      return;
    generation = newGeneration;
    firstRequireInst = nullptr;
  }

public:
  SILInstruction *getInst(unsigned callerGeneration) {
    resetIfNew(callerGeneration);
    return firstRequireInst;
  }

  void setInst(unsigned callerGeneration, SILInstruction *newValue) {
    resetIfNew(callerGeneration);
    firstRequireInst = newValue;
  }
};

/// We only want to emit errors for the first requires along a path from a
/// transfer instruction. We discover this by walking from user blocks to
struct RequireLiveness {
  unsigned generation;
  SILInstruction *transferInst;
  BasicBlockData<BlockLivenessInfo> &blockLivenessInfo;
  InstructionSet allRequires;
  InstructionSetWithSize finalRequires;

  /// If we have requires in the def block before our transfer, this is the
  /// first require.
  SILInstruction *firstRequireBeforeTransferInDefBlock = nullptr;

  RequireLiveness(unsigned generation, Operand *transferOp,
                  BasicBlockData<BlockLivenessInfo> &blockLivenessInfo)
      : generation(generation), transferInst(transferOp->getUser()),
        blockLivenessInfo(blockLivenessInfo),
        allRequires(transferOp->getParentFunction()),
        finalRequires(transferOp->getParentFunction()) {}

  template <typename Collection>
  void process(Collection collection);

  /// Attempt to process requireInst for our def block. Returns false if
  /// requireInst was before our def and we need to do interprocedural
  /// processing. Returns true if requireInst was after our transferInst and we
  /// were able to appropriately determine if we should emit it or not.
  void processDefBlock();

  /// Process all requires in block, updating blockLivenessInfo.
  void processNonDefBlock(SILBasicBlock *block);
};

} // namespace

void RequireLiveness::processDefBlock() {
  LLVM_DEBUG(llvm::dbgs() << "    Processing def block!\n");
  // First walk from the beginning of the block to the transfer instruction to
  // see if we have any requires before our def. Once we find one, we can skip
  // the traversal and jump straight to the transfer.
  for (auto ii = transferInst->getParent()->begin(),
            ie = transferInst->getIterator();
       ii != ie; ++ii) {
    if (allRequires.contains(&*ii) && !firstRequireBeforeTransferInDefBlock) {
      firstRequireBeforeTransferInDefBlock = &*ii;
      LLVM_DEBUG(llvm::dbgs() << "        Found transfer before def: "
                              << *firstRequireBeforeTransferInDefBlock);
      break;
    }
  }

  // Then walk from our transferInst to the end of the block looking for the
  // first require inst. Once we find it... return.
  for (auto ii = std::next(transferInst->getIterator()),
            ie = transferInst->getParent()->end();
       ii != ie; ++ii) {
    if (!allRequires.contains(&*ii))
      continue;

    finalRequires.insert(&*ii);
    LLVM_DEBUG(llvm::dbgs() << "        Found transfer after def: " << *ii);
    return;
  }
}

void RequireLiveness::processNonDefBlock(SILBasicBlock *block) {
  // Walk from the bottom to the top... assigning to our block state.
  auto blockState = blockLivenessInfo.get(block);
  for (auto &inst : llvm::make_range(block->rbegin(), block->rend())) {
    if (!finalRequires.contains(&inst))
      continue;
    blockState.get()->setInst(generation, &inst);
  }
}

template <typename Collection>
void RequireLiveness::process(Collection requireInstList) {
  LLVM_DEBUG(llvm::dbgs() << "==> Performing Require Liveness for: "
                          << *transferInst);

  // Then put all of our requires into our allRequires set.
  BasicBlockWorklist initializingWorklist(transferInst->getFunction());
  for (auto *require : requireInstList) {
    LLVM_DEBUG(llvm::dbgs() << "        Require Inst: " << *require);
    allRequires.insert(require);
    initializingWorklist.pushIfNotVisited(require->getParent());
  }

  // Then process our def block to see if we have any requires before and after
  // the transferInst...
  processDefBlock();

  // If we found /any/ requries after the transferInst, we can bail early since
  // that is guaranteed to dominate all further requires.
  if (!finalRequires.empty()) {
    LLVM_DEBUG(
        llvm::dbgs()
        << "        Found transfer after def in def block! Exiting early!\n");
    return;
  }

  LLVM_DEBUG(llvm::dbgs() << "        Did not find transfer after def in def "
                             "block! Walking blocks!\n");

  // If we found a transfer in the def block before our def, add it to the block
  // state for the def.
  if (firstRequireBeforeTransferInDefBlock) {
    LLVM_DEBUG(
        llvm::dbgs()
        << "        Found a require before transfer! Adding to block state!\n");
    auto blockState = blockLivenessInfo.get(transferInst->getParent());
    blockState.get()->setInst(generation, firstRequireBeforeTransferInDefBlock);
  }

  // Then for each require block that isn't a def block transfer, find the
  // earliest transfer inst.
  while (auto *requireBlock = initializingWorklist.pop()) {
    auto blockState = blockLivenessInfo.get(requireBlock);
    for (auto &inst : *requireBlock) {
      if (!allRequires.contains(&inst))
        continue;
      LLVM_DEBUG(llvm::dbgs() << "        Mapping Block bb"
                              << requireBlock->getDebugID() << " to: " << inst);
      blockState.get()->setInst(generation, &inst);
      break;
    }
  }

  // Then walk from our def block looking for setInst blocks.
  auto *transferBlock = transferInst->getParent();
  BasicBlockWorklist worklist(transferInst->getFunction());
  for (auto *succBlock : transferBlock->getSuccessorBlocks())
    worklist.pushIfNotVisited(succBlock);

  while (auto *next = worklist.pop()) {
    // Check if we found an earliest requires... if so, add that to final
    // requires and continue. We don't want to visit successors.
    auto blockState = blockLivenessInfo.get(next);
    if (auto *inst = blockState.get()->getInst(generation)) {
      finalRequires.insert(inst);
      continue;
    }

    // Do not look at successors of the transfer block.
    if (next == transferBlock)
      continue;

    // Otherwise, we did not find a requires and need to search further
    // successors.
    for (auto *succBlock : next->getSuccessorBlocks())
      worklist.pushIfNotVisited(succBlock);
  }
}

//===----------------------------------------------------------------------===//
//                MARK: UseAfterTransfer Diagnostic Inference
//===----------------------------------------------------------------------===//

namespace {

class UseAfterTransferDiagnosticInferrer {
public:
  enum class UseDiagnosticInfoKind {
    Invalid = 0,

    /// Used if we have an isolation crossing for our error.
    IsolationCrossing = 1,

    /// In certain cases, we think a race can happen, but we couldn't find the
    /// isolation crossing specifically to emit a better error. Still emit an
    /// error though.
    RaceWithoutKnownIsolationCrossing = 2,

    /// Used if the error is due to a transfer into an assignment into a
    /// transferring parameter.
    AssignmentIntoTransferringParameter = 3,

    /// Set to true if this is a use of a normal value that was strongly
    /// transferred.
    UseOfStronglyTransferredValue = 4,

    /// We transferred the value by capturing the value in an isolated closure.
    IsolationCrossingDueToCapture = 5,
  };

  class UseDiagnosticInfo {
    UseDiagnosticInfoKind kind;
    std::optional<ApplyIsolationCrossing> isolationCrossing;

  public:
    static UseDiagnosticInfo
    forIsolationCrossing(ApplyIsolationCrossing isolationCrossing) {
      return UseDiagnosticInfo(UseDiagnosticInfoKind::IsolationCrossing,
                               isolationCrossing);
    }

    static UseDiagnosticInfo
    forIsolationCrossingDueToCapture(ApplyIsolationCrossing isolationCrossing) {
      return UseDiagnosticInfo(
          UseDiagnosticInfoKind::IsolationCrossingDueToCapture,
          isolationCrossing);
    }

    static UseDiagnosticInfo forIsolationCrossingWithUnknownIsolation() {
      return UseDiagnosticInfo(
          UseDiagnosticInfoKind::RaceWithoutKnownIsolationCrossing, {});
    }

    static UseDiagnosticInfo forAssignmentIntoTransferringParameter() {
      return UseDiagnosticInfo(
          UseDiagnosticInfoKind::AssignmentIntoTransferringParameter, {});
    }

    static UseDiagnosticInfo forUseOfStronglyTransferredValue() {
      return UseDiagnosticInfo(
          UseDiagnosticInfoKind::UseOfStronglyTransferredValue, {});
    }

    UseDiagnosticInfoKind getKind() const { return kind; }

    ApplyIsolationCrossing getIsolationCrossing() const {
      // assert(isolationCrossing && "Isolation crossing must be non-null");
      return isolationCrossing.value();
    }

  private:
    UseDiagnosticInfo(UseDiagnosticInfoKind kind,
                      std::optional<ApplyIsolationCrossing> isolationCrossing)
        : kind(kind), isolationCrossing(isolationCrossing) {}
  };

  struct ApplyUse {
    SILLocation loc;
    Type inferredType;
    UseDiagnosticInfo diagInfo;

    ApplyUse(SILLocation loc, Type inferredType, UseDiagnosticInfo diagInfo)
        : loc(loc), inferredType(inferredType), diagInfo(diagInfo) {}
  };

private:
  RegionAnalysisValueMap &valueMap;
  SILLocation baseLoc = SILLocation::invalid();
  Type baseInferredType;

  /// If one does not have a better loc or type, please use baseUse and
  /// baseInferredType.
  SmallVector<ApplyUse, 4> applyUses;

  struct Walker;

public:
  UseAfterTransferDiagnosticInferrer(RegionAnalysisValueMap &valueMap)
      : valueMap(valueMap) {}
  void init(const Operand *op);

  void addApplyUse(UseDiagnosticInfo diagnosticInfo) {
    applyUses.emplace_back(baseLoc, baseInferredType, diagnosticInfo);
  }

  void addApplyUse(SILLocation loc, UseDiagnosticInfo diagnosticInfo) {
    applyUses.emplace_back(loc, baseInferredType, diagnosticInfo);
  }

  void addApplyUse(Type type, UseDiagnosticInfo diagnosticInfo) {
    applyUses.emplace_back(baseLoc, type, diagnosticInfo);
  }

  ArrayRef<ApplyUse> getApplyUses() const { return applyUses; }

private:
  /// Init for an apply that does not have an associated apply expr.
  ///
  /// This should only occur when writing SIL test cases today. In the future,
  /// we may represent all of the actor isolation information at the SIL level,
  /// but we are not there yet today.
  void initForApply(ApplyIsolationCrossing isolationCrossing);
  bool initForIsolatedPartialApply(Operand *op, AbstractClosureExpr *ace);

  void initForApply(const Operand *op, ApplyExpr *expr);
  void initForAutoclosure(const Operand *op, AutoClosureExpr *expr);

  void initForAssignmentToTransferringParameter(const Operand *op) {
    addApplyUse(op->get()->getType().getASTType(),
                UseDiagnosticInfo::forAssignmentIntoTransferringParameter());
  }

  void initForUseOfStronglyTransferredValue(const Operand *op) {
    addApplyUse(op->get()->getType().getASTType(),
                UseDiagnosticInfo::forUseOfStronglyTransferredValue());
  }

  Expr *getFoundExprForSelf(ApplyExpr *sourceApply) {
    if (auto callExpr = dyn_cast<CallExpr>(sourceApply))
      if (auto calledExpr =
              dyn_cast<DotSyntaxCallExpr>(callExpr->getDirectCallee()))
        return calledExpr->getBase();
    return nullptr;
  }

  Expr *getFoundExprForParam(ApplyExpr *sourceApply, unsigned argNum) {
    auto *expr = sourceApply->getArgs()->getExpr(argNum);

    // If we have an erasure expression, lets use the original type. We do
    // this since we are not saying the specific parameter that is the
    // issue and we are using the type to explain it to the user.
    if (auto *erasureExpr = dyn_cast<ErasureExpr>(expr))
      expr = erasureExpr->getSubExpr();

    return expr;
  }
};

} // namespace

bool UseAfterTransferDiagnosticInferrer::initForIsolatedPartialApply(
    Operand *op, AbstractClosureExpr *ace) {
  SmallVector<std::tuple<CapturedValue, unsigned, ApplyIsolationCrossing>, 8>
      foundCapturedIsolationCrossing;
  ace->getIsolationCrossing(foundCapturedIsolationCrossing);
  if (foundCapturedIsolationCrossing.empty())
    return false;

  unsigned opIndex = ApplySite(op->getUser()).getAppliedArgIndex(*op);
  for (auto &p : foundCapturedIsolationCrossing) {
    if (std::get<1>(p) == opIndex) {
      addApplyUse(
          RegularLocation(std::get<0>(p).getLoc()),
          UseDiagnosticInfo::forIsolationCrossingDueToCapture(std::get<2>(p)));
      return true;
    }
  }

  return false;
}

void UseAfterTransferDiagnosticInferrer::initForApply(
    ApplyIsolationCrossing isolationCrossing) {
  addApplyUse(UseDiagnosticInfo::forIsolationCrossing(isolationCrossing));
}

void UseAfterTransferDiagnosticInferrer::initForApply(const Operand *op,
                                                      ApplyExpr *sourceApply) {
  auto isolationCrossing = sourceApply->getIsolationCrossing().value();

  // Grab out full apply site and see if we can find a better expr.
  SILInstruction *i = const_cast<SILInstruction *>(op->getUser());
  auto fai = FullApplySite::isa(i);

  Expr *foundExpr = nullptr;

  // If we have self, then infer it.
  if (fai.hasSelfArgument() && op == &fai.getSelfArgumentOperand()) {
    foundExpr = getFoundExprForSelf(sourceApply);
  } else {
    // Otherwise, try to infer using the operand of the ApplyExpr.
    unsigned argNum = [&]() -> unsigned {
      if (fai.isCalleeOperand(*op))
        return op->getOperandNumber();
      return fai.getAppliedArgIndex(*op);
    }();
    assert(argNum < sourceApply->getArgs()->size());
    foundExpr = getFoundExprForParam(sourceApply, argNum);
  }

  auto inferredArgType =
      foundExpr ? foundExpr->findOriginalType() : baseInferredType;
  addApplyUse(inferredArgType,
              UseDiagnosticInfo::forIsolationCrossing(isolationCrossing));
}

struct UseAfterTransferDiagnosticInferrer::Walker : ASTWalker {
  UseAfterTransferDiagnosticInferrer &foundTypeInfo;
  ValueDecl *targetDecl;
  SmallPtrSet<Expr *, 8> visitedCallExprDeclRefExprs;

  Walker(UseAfterTransferDiagnosticInferrer &foundTypeInfo,
         ValueDecl *targetDecl)
      : foundTypeInfo(foundTypeInfo), targetDecl(targetDecl) {}

  Expr *lookThroughExpr(Expr *expr) {
    while (true) {
      if (auto *memberRefExpr = dyn_cast<MemberRefExpr>(expr)) {
        expr = memberRefExpr->getBase();
        continue;
      }

      if (auto *cvt = dyn_cast<ImplicitConversionExpr>(expr)) {
        expr = cvt->getSubExpr();
        continue;
      }

      if (auto *e = dyn_cast<ForceValueExpr>(expr)) {
        expr = e->getSubExpr();
        continue;
      }

      if (auto *t = dyn_cast<TupleElementExpr>(expr)) {
        expr = t->getBase();
        continue;
      }

      return expr;
    }
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    if (auto *declRef = dyn_cast<DeclRefExpr>(expr)) {
      // If this decl ref expr was not visited as part of a callExpr, add it as
      // something without isolation crossing.
      if (!visitedCallExprDeclRefExprs.count(declRef)) {
        if (declRef->getDecl() == targetDecl) {
          visitedCallExprDeclRefExprs.insert(declRef);
          foundTypeInfo.addApplyUse(
              declRef->findOriginalType(),
              UseDiagnosticInfo::forIsolationCrossingWithUnknownIsolation());
          return Action::Continue(expr);
        }
      }
    }

    if (auto *callExpr = dyn_cast<CallExpr>(expr)) {
      if (auto isolationCrossing = callExpr->getIsolationCrossing()) {
        // Search callExpr's arguments to see if we have our targetDecl.
        auto *argList = callExpr->getArgs();
        for (auto pair : llvm::enumerate(argList->getArgExprs())) {
          auto *arg = lookThroughExpr(pair.value());
          if (auto *declRef = dyn_cast<DeclRefExpr>(arg)) {
            if (declRef->getDecl() == targetDecl) {
              // Found our target!
              visitedCallExprDeclRefExprs.insert(declRef);
              foundTypeInfo.addApplyUse(
                  declRef->findOriginalType(),
                  UseDiagnosticInfo::forIsolationCrossing(*isolationCrossing));
              return Action::Continue(expr);
            }
          }
        }
      }
    }

    return Action::Continue(expr);
  }
};

static SILValue getDestOfStoreOrCopyAddr(Operand *op) {
  if (auto *si = dyn_cast<StoreInst>(op->getUser()))
    return si->getDest();

  if (auto *copyAddr = dyn_cast<CopyAddrInst>(op->getUser()))
    return copyAddr->getDest();

  return SILValue();
}

void UseAfterTransferDiagnosticInferrer::init(const Operand *op) {
  baseLoc = op->getUser()->getLoc();
  baseInferredType = op->get()->getType().getASTType();
  auto *nonConstOp = const_cast<Operand *>(op);

  // Before we do anything, see if the transfer instruction was into a
  // transferring parameter alloc_stack. In such a case, we emit a special
  // message.
  if (auto destValue = getDestOfStoreOrCopyAddr(nonConstOp)) {
    auto trackedValue = valueMap.getTrackableValue(destValue);
    if (trackedValue.isTransferringParameter()) {
      return initForAssignmentToTransferringParameter(op);
    }
  }

  // Otherwise, see if our operand's instruction is a transferring parameter.
  if (auto fas = FullApplySite::isa(nonConstOp->getUser())) {
    if (!fas.getArgumentConvention(*nonConstOp).isIndirectOutParameter()) {
      if (fas.getArgumentParameterInfo(*nonConstOp)
          .hasOption(SILParameterInfo::Transferring)) {
        return initForUseOfStronglyTransferredValue(op);
      }
    }
  }

  auto loc = op->getUser()->getLoc();

  // If we have a partial_apply that is actor isolated, see if we found a
  // transfer error due to us transferring a value into it.
  if (auto *ace = loc.getAsASTNode<AbstractClosureExpr>()) {
    if (ace->getActorIsolation().isActorIsolated()) {
      if (initForIsolatedPartialApply(nonConstOp, ace)) {
        return;
      }
    }
  }

  if (auto *sourceApply = loc.getAsASTNode<ApplyExpr>()) {
    return initForApply(op, sourceApply);
  }

  if (auto fas = FullApplySite::isa(nonConstOp->getUser())) {
    if (auto isolationCrossing = fas.getIsolationCrossing()) {
      return initForApply(*isolationCrossing);
    }
  }

  auto *autoClosureExpr = loc.getAsASTNode<AutoClosureExpr>();
  if (!autoClosureExpr) {
    diagnoseError(op->getUser(), diag::regionbasedisolation_unknown_pattern);
    return;
  }

  auto *i = const_cast<SILInstruction *>(op->getUser());
  auto pai = ApplySite::isa(i);
  unsigned captureIndex = pai.getAppliedArgIndex(*op);

  auto captureInfo =
      autoClosureExpr->getCaptureInfo().getCaptures()[captureIndex];
  auto *captureDecl = captureInfo.getDecl();
  Walker walker(*this, captureDecl);
  autoClosureExpr->walk(walker);
}

//===----------------------------------------------------------------------===//
//            MARK: Transfer NonTransferrable Diagnostic Inference
//===----------------------------------------------------------------------===//

namespace {

struct TransferredNonTransferrableInfo {
  /// The use that actually caused the transfer.
  Operand *transferredOperand;

  /// The non-transferrable value that is in the same region as \p
  /// transferredOperand.get().
  SILValue nonTransferrableValue;

  TransferredNonTransferrableInfo(Operand *transferredOperand,
                                  SILValue nonTransferrableValue)
      : transferredOperand(transferredOperand),
        nonTransferrableValue(nonTransferrableValue) {}
};

class TransferNonTransferrableDiagnosticInferrer {
public:
  enum class UseDiagnosticInfoKind {
    Invalid = 0,

    /// Used if we have a use that we haven't categorized so we emit the generic
    /// call site is self error.
    MiscUse = 1,

    /// Used if we have a function argument that is transferred into an apply.
    FunctionArgumentApply = 2,

    /// Used if we have a function argument that is transferred into an closure.
    FunctionArgumentClosure = 3,

    /// Used if we have a function argument passed as an explicitly strongly
    /// transferring argument to a function.
    FunctionArgumentApplyStronglyTransferred = 4,
  };

  struct UseDiagnosticInfo {
    UseDiagnosticInfoKind kind;
    std::optional<ActorIsolation> transferredIsolation;

    static UseDiagnosticInfo forMiscUse() {
      return {UseDiagnosticInfoKind::MiscUse, {}};
    }

    static UseDiagnosticInfo
    forFunctionArgumentApply(ActorIsolation isolation) {
      return {UseDiagnosticInfoKind::FunctionArgumentApply, isolation};
    }

    static UseDiagnosticInfo
    forFunctionArgumentClosure(ActorIsolation isolation) {
      return {UseDiagnosticInfoKind::FunctionArgumentClosure, isolation};
    }

    static UseDiagnosticInfo forFunctionArgumentApplyStronglyTransferred() {
      return {UseDiagnosticInfoKind::FunctionArgumentApplyStronglyTransferred,
              {}};
    }

  private:
    UseDiagnosticInfo(UseDiagnosticInfoKind kind,
                      std::optional<ActorIsolation> isolation)
        : kind(kind), transferredIsolation(isolation) {}
  };

private:
  TransferredNonTransferrableInfo info;
  std::optional<UseDiagnosticInfo> diagnosticInfo;
  SourceLoc loc;

public:
  TransferNonTransferrableDiagnosticInferrer(
      TransferredNonTransferrableInfo info)
      : info(info),
        loc(info.transferredOperand->getUser()->getLoc().getSourceLoc()) {}

  /// Gathers diagnostics. Returns false if we emitted a "I don't understand
  /// error". If we emit such an error, we should bail without emitting any
  /// further diagnostics, since we may not have any diagnostics or be in an
  /// inconcistent state.
  bool run();

  UseDiagnosticInfo getDiagnostic() const { return diagnosticInfo.value(); }
  SourceLoc getLoc() const { return loc; }

private:
  bool initForIsolatedPartialApply(Operand *op, AbstractClosureExpr *ace);
};

} // namespace

bool TransferNonTransferrableDiagnosticInferrer::initForIsolatedPartialApply(
    Operand *op, AbstractClosureExpr *ace) {
  SmallVector<std::tuple<CapturedValue, unsigned, ApplyIsolationCrossing>, 8>
      foundCapturedIsolationCrossing;
  ace->getIsolationCrossing(foundCapturedIsolationCrossing);
  if (foundCapturedIsolationCrossing.empty())
    return false;

  unsigned opIndex = ApplySite(op->getUser()).getAppliedArgIndex(*op);
  for (auto &p : foundCapturedIsolationCrossing) {
    if (std::get<1>(p) == opIndex) {
      loc = std::get<0>(p).getLoc();
      diagnosticInfo = UseDiagnosticInfo::forFunctionArgumentClosure(
          std::get<2>(p).getCalleeIsolation());
      return true;
    }
  }

  return false;
}

bool TransferNonTransferrableDiagnosticInferrer::run() {
  if (!isa<SILFunctionArgument>(info.nonTransferrableValue)) {
    diagnosticInfo = UseDiagnosticInfo::forMiscUse();
    return true;
  }

  // We need to find the isolation info.
  auto loc = info.transferredOperand->getUser()->getLoc();

  if (auto *sourceApply = loc.getAsASTNode<ApplyExpr>()) {
    std::optional<ActorIsolation> isolation = {};

    // First try to get the apply from the isolation crossing.
    if (auto value = sourceApply->getIsolationCrossing())
      isolation = value->getCalleeIsolation();

    // If we could not infer an isolation...
    if (!isolation) {
      // First see if we have a transferring argument.
      if (auto fas = FullApplySite::isa(info.transferredOperand->getUser())) {
        if (fas.getArgumentParameterInfo(*info.transferredOperand)
                .hasOption(SILParameterInfo::Transferring)) {
          diagnosticInfo =
              UseDiagnosticInfo::forFunctionArgumentApplyStronglyTransferred();
          return true;
        }
      }

      // Otherwise, emit a "we don't know error" that tells the user to file a
      // bug.
      diagnoseError(info.transferredOperand->getUser(),
                    diag::regionbasedisolation_unknown_pattern);
      return false;
    }

    diagnosticInfo = UseDiagnosticInfo::forFunctionArgumentApply(*isolation);
    return true;
  }

  if (auto *ace = loc.getAsASTNode<AbstractClosureExpr>()) {
    if (ace->getActorIsolation().isActorIsolated()) {
      if (initForIsolatedPartialApply(info.transferredOperand, ace)) {
        return true;
      }
    }
  }

  diagnosticInfo = UseDiagnosticInfo::forMiscUse();
  return true;
}

//===----------------------------------------------------------------------===//
//                         MARK: Diagnostic Emission
//===----------------------------------------------------------------------===//

namespace {

struct DiagnosticEvaluator final
    : PartitionOpEvaluatorBaseImpl<DiagnosticEvaluator> {
  RegionAnalysisFunctionInfo *info;
  SmallFrozenMultiMap<Operand *, SILInstruction *, 8>
      &transferOpToRequireInstMultiMap;

  /// First value is the operand that was transferred... second value is the
  /// non-transferrable value in the same region as that value. The second value
  /// is what is non-transferrable.
  SmallVectorImpl<TransferredNonTransferrableInfo> &transferredNonTransferrable;

  DiagnosticEvaluator(Partition &workingPartition,
                      RegionAnalysisFunctionInfo *info,
                      SmallFrozenMultiMap<Operand *, SILInstruction *, 8>
                          &transferOpToRequireInstMultiMap,
                      SmallVectorImpl<TransferredNonTransferrableInfo>
                          &transferredNonTransferrable)
      : PartitionOpEvaluatorBaseImpl(workingPartition,
                                     info->getOperandSetFactory()),
        info(info),
        transferOpToRequireInstMultiMap(transferOpToRequireInstMultiMap),
        transferredNonTransferrable(transferredNonTransferrable) {}

  void handleFailure(const PartitionOp &partitionOp,
                     TrackableValueID transferredVal,
                     TransferringOperand transferringOp) const {
    // Ignore this if we have a gep like instruction that is returning a
    // sendable type and transferringOp was not set with closure
    // capture.
    if (auto *svi =
            dyn_cast<SingleValueInstruction>(partitionOp.getSourceInst())) {
      if (isa<TupleElementAddrInst, StructElementAddrInst>(svi) &&
          !regionanalysisimpl::isNonSendableType(svi->getType(),
                                                 svi->getFunction())) {
        bool isCapture = transferringOp.isClosureCaptured();
        if (!isCapture) {
          return;
        }
      }
    }

    auto rep = info->getValueMap().getRepresentative(transferredVal);
    LLVM_DEBUG(llvm::dbgs()
               << "    Emitting Use After Transfer Error!\n"
               << "        ID:  %%" << transferredVal << "\n"
               << "        Rep: " << *rep
               << "        Require Inst: " << *partitionOp.getSourceInst()
               << "        Transferring Op Num: "
               << transferringOp.getOperand()->getOperandNumber() << '\n'
               << "        Transferring Inst: " << *transferringOp.getUser());
    transferOpToRequireInstMultiMap.insert(transferringOp.getOperand(),
                                           partitionOp.getSourceInst());
  }

  ArrayRef<Element> getNonTransferrableElements() const {
    return info->getValueMap().getNonTransferrableElements();
  }

  void handleTransferNonTransferrable(const PartitionOp &partitionOp,
                                      TrackableValueID transferredVal) const {
    LLVM_DEBUG(llvm::dbgs()
               << "    Emitting TransferNonTransferrable Error!\n"
               << "        ID:  %%" << transferredVal << "\n"
               << "        Rep: "
               << *info->getValueMap().getRepresentative(transferredVal));
    auto *self = const_cast<DiagnosticEvaluator *>(this);
    auto nonTransferrableValue =
        info->getValueMap().getRepresentative(transferredVal);
    self->transferredNonTransferrable.emplace_back(partitionOp.getSourceOp(),
                                                   nonTransferrableValue);
  }

  bool isActorDerived(Element element) const {
    return info->getValueMap().isActorDerived(element);
  }

  bool isClosureCaptured(Element element, Operand *op) const {
    auto value = info->getValueMap().maybeGetRepresentative(element);
    if (!value)
      return false;
    return info->isClosureCaptured(value, op);
  }
};

} // namespace

namespace {

class TransferNonSendableImpl {
  RegionAnalysisFunctionInfo *regionInfo;
  SmallFrozenMultiMap<Operand *, SILInstruction *, 8>
      transferOpToRequireInstMultiMap;
  SmallVector<TransferredNonTransferrableInfo, 8> transferredNonTransferrable;

public:
  TransferNonSendableImpl(RegionAnalysisFunctionInfo *regionInfo)
      : regionInfo(regionInfo) {}

  void run();

private:
  void runDiagnosticEvaluator();

  void emitUseAfterTransferDiagnostics();
  void emitTransferredNonTransferrableDiagnostics();
};

} // namespace

void TransferNonSendableImpl::runDiagnosticEvaluator() {
  // Then for each block...
  LLVM_DEBUG(llvm::dbgs() << "Walking blocks for diagnostics.\n");
  for (auto [block, blockState] : regionInfo->getRange()) {
    LLVM_DEBUG(llvm::dbgs() << "|--> Block bb" << block.getDebugID() << "\n");
    LLVM_DEBUG(llvm::dbgs() << "Entry Partition: ";
               blockState.getEntryPartition().print(llvm::dbgs()));

    // Grab its entry partition and setup an evaluator for the partition that
    // has callbacks that emit diagnsotics...
    Partition workingPartition = blockState.getEntryPartition();
    DiagnosticEvaluator eval(workingPartition, regionInfo,
                             transferOpToRequireInstMultiMap,
                             transferredNonTransferrable);

    // And then evaluate all of our partition ops on the entry partition.
    for (auto &partitionOp : blockState.getPartitionOps()) {
      eval.apply(partitionOp);
    }

    LLVM_DEBUG(llvm::dbgs() << "Exit Partition: ";
               workingPartition.print(llvm::dbgs()));
  }

  LLVM_DEBUG(llvm::dbgs() << "Finished walking blocks for diagnostics.\n");

  // Now that we have found all of our transferInsts/Requires emit errors.
  transferOpToRequireInstMultiMap.setFrozen();
}

void TransferNonSendableImpl::emitUseAfterTransferDiagnostics() {
  auto *function = regionInfo->getFunction();
  BasicBlockData<BlockLivenessInfo> blockLivenessInfo(function);
  // We use a generation counter so we can lazily reset blockLivenessInfo
  // since we cannot clear it without iterating over it.
  unsigned blockLivenessInfoGeneration = 0;

  if (transferOpToRequireInstMultiMap.empty())
    return;

  LLVM_DEBUG(llvm::dbgs()
             << "Visiting found transfer+[requireInsts] for diagnostics.\n");
  auto &astContext = regionInfo->getFunction()->getASTContext();
  for (auto [transferOp, requireInsts] :
       transferOpToRequireInstMultiMap.getRange()) {

    LLVM_DEBUG(llvm::dbgs()
               << "Transfer Op. Number: " << transferOp->getOperandNumber()
               << ". User: " << *transferOp->getUser());

    RequireLiveness liveness(blockLivenessInfoGeneration, transferOp,
                             blockLivenessInfo);
    ++blockLivenessInfoGeneration;
    liveness.process(requireInsts);

    UseAfterTransferDiagnosticInferrer diagnosticInferrer(
        regionInfo->getValueMap());
    diagnosticInferrer.init(transferOp);

    // If we were supposed to emit an error and we failed to do so, emit a
    // hard error so that the user knows to file a bug.
    //
    // DISCUSSION: We do this rather than asserting since users often times do
    // not know what to do if the compiler crashes. This at least shows up in
    // editor UIs providing a more actionable error message.
    auto applyUses = diagnosticInferrer.getApplyUses();
    if (applyUses.empty()) {
      diagnoseError(transferOp, diag::regionbasedisolation_unknown_pattern);
      continue;
    }

    using UseDiagnosticInfoKind =
        UseAfterTransferDiagnosticInferrer::UseDiagnosticInfoKind;
    for (auto &info : applyUses) {
      switch (info.diagInfo.getKind()) {
      case UseDiagnosticInfoKind::Invalid:
        llvm_unreachable("Should never see this!");
      case UseDiagnosticInfoKind::IsolationCrossing: {
        auto isolation = info.diagInfo.getIsolationCrossing();
        diagnoseError(
            astContext, info.loc,
            diag::regionbasedisolation_transfer_yields_race_with_isolation,
            info.inferredType, isolation.getCallerIsolation(),
            isolation.getCalleeIsolation())
            .highlight(info.loc.getSourceRange());
        break;
      }
      case UseDiagnosticInfoKind::RaceWithoutKnownIsolationCrossing:
        diagnoseError(
            astContext, info.loc,
            diag::regionbasedisolation_transfer_yields_race_no_isolation,
            info.inferredType)
            .highlight(info.loc.getSourceRange());
        break;
      case UseDiagnosticInfoKind::UseOfStronglyTransferredValue:
        diagnoseError(
            astContext, info.loc,
            diag::
                regionbasedisolation_transfer_yields_race_stronglytransferred_binding,
            info.inferredType)
            .highlight(info.loc.getSourceRange());
        break;
      case UseDiagnosticInfoKind::AssignmentIntoTransferringParameter:
        diagnoseError(
            astContext, info.loc,
            diag::
                regionbasedisolation_transfer_yields_race_transferring_parameter,
            info.inferredType)
            .highlight(info.loc.getSourceRange());
        break;
      case UseDiagnosticInfoKind::IsolationCrossingDueToCapture:
        auto isolation = info.diagInfo.getIsolationCrossing();
        diagnoseError(astContext, info.loc,
                      diag::regionbasedisolation_isolated_capture_yields_race,
                      info.inferredType, isolation.getCalleeIsolation(),
                      isolation.getCallerIsolation())
            .highlight(info.loc.getSourceRange());
        break;
      }

      // Ok, we now have our requires... emit the errors.
      bool didEmitRequire = false;

      InstructionSet requireInstsUnique(function);
      for (auto *require : requireInsts) {
        // We can have multiple of the same require insts if we had a require
        // and an assign from the same instruction. Our liveness checking
        // above doesn't care about that, but we still need to make sure we do
        // not emit twice.
        if (!requireInstsUnique.insert(require))
          continue;

        // If this was not a last require, do not emit an error.
        if (!liveness.finalRequires.contains(require))
          continue;

        diagnoseNote(require, diag::regionbasedisolation_maybe_race)
            .highlight(require->getLoc().getSourceRange());
        didEmitRequire = true;
      }

      assert(didEmitRequire && "Should have a require for all errors?!");
    }
  }
}

void TransferNonSendableImpl::emitTransferredNonTransferrableDiagnostics() {
  if (transferredNonTransferrable.empty())
    return;

  LLVM_DEBUG(
      llvm::dbgs() << "Emitting transfer non transferrable diagnostics.\n");

  using UseDiagnosticInfoKind =
      TransferNonTransferrableDiagnosticInferrer::UseDiagnosticInfoKind;

  auto &astContext = regionInfo->getFunction()->getASTContext();
  for (auto info : transferredNonTransferrable) {
    auto *op = info.transferredOperand;
    TransferNonTransferrableDiagnosticInferrer diagnosticInferrer(info);
    if (!diagnosticInferrer.run())
      continue;

    auto diagnosticInfo = diagnosticInferrer.getDiagnostic();
    auto loc = diagnosticInferrer.getLoc();
    switch (diagnosticInfo.kind) {
    case UseDiagnosticInfoKind::Invalid:
      llvm_unreachable("Should never see this");
    case UseDiagnosticInfoKind::MiscUse:
      diagnoseError(astContext, loc,
                    diag::regionbasedisolation_selforargtransferred);
      break;
    case UseDiagnosticInfoKind::FunctionArgumentApply: {
      diagnoseError(astContext, loc, diag::regionbasedisolation_arg_transferred,
                    op->get()->getType().getASTType(),
                    diagnosticInfo.transferredIsolation.value())
          .highlight(op->getUser()->getLoc().getSourceRange());
      // Only emit the note if our value is different from the function
      // argument.
      auto rep = regionInfo->getValueMap()
                     .getTrackableValue(op->get())
                     .getRepresentative();
      if (rep.maybeGetValue() == info.nonTransferrableValue)
        continue;
      auto *fArg = cast<SILFunctionArgument>(info.nonTransferrableValue);
      diagnoseNote(
          astContext, fArg->getDecl()->getLoc(),
          diag::regionbasedisolation_isolated_since_in_same_region_basename,
          "task isolated", fArg->getDecl()->getBaseName());
      break;
    }
    case UseDiagnosticInfoKind::FunctionArgumentClosure: {
      diagnoseError(astContext, loc, diag::regionbasedisolation_arg_transferred,
                    op->get()->getType().getASTType(),
                    diagnosticInfo.transferredIsolation.value())
          .highlight(op->getUser()->getLoc().getSourceRange());
      // Only emit the note if our value is different from the function
      // argument.
      auto rep = regionInfo->getValueMap()
                     .getTrackableValue(op->get())
                     .getRepresentative();
      if (rep.maybeGetValue() == info.nonTransferrableValue)
        continue;
      auto *fArg = cast<SILFunctionArgument>(info.nonTransferrableValue);
      diagnoseNote(
          astContext, fArg->getDecl()->getLoc(),
          diag::regionbasedisolation_isolated_since_in_same_region_basename,
          "task isolated", fArg->getDecl()->getBaseName());
      break;
    }
    case UseDiagnosticInfoKind::FunctionArgumentApplyStronglyTransferred: {
      diagnoseError(
          astContext, loc,
          diag::regionbasedisolation_arg_passed_to_strongly_transferred_param,
          op->get()->getType().getASTType())
          .highlight(op->getUser()->getLoc().getSourceRange());
      break;
    }
    }
  }
}

/// Once we have reached a fixpoint, this routine runs over all blocks again
/// reporting any failures by applying our ops to the converged dataflow
/// state.
void TransferNonSendableImpl::run() {
  auto *function = regionInfo->getFunction();
  LLVM_DEBUG(llvm::dbgs() << "Emitting diagnostics for function "
                          << function->getName() << "\n");

  runDiagnosticEvaluator();
  emitTransferredNonTransferrableDiagnostics();
  emitUseAfterTransferDiagnostics();
}

//===----------------------------------------------------------------------===//
//                         MARK: Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class TransferNonSendable : public SILFunctionTransform {
  void run() override {
    SILFunction *function = getFunction();

    auto *functionInfo = getAnalysis<RegionAnalysis>()->get(function);
    if (!functionInfo->isSupportedFunction()) {
      LLVM_DEBUG(llvm::dbgs() << "===> SKIPPING UNSUPPORTED FUNCTION: "
                              << function->getName() << '\n');

      return;
    }

    LLVM_DEBUG(llvm::dbgs()
               << "===> PROCESSING: " << function->getName() << '\n');

    TransferNonSendableImpl impl(functionInfo);
    impl.run();
  }
};

} // end anonymous namespace

SILTransform *swift::createTransferNonSendable() {
  return new TransferNonSendable();
}
