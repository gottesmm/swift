//===--- SILOwnershipVerifier.cpp -----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "silverifier"
#include "swift/AST/ASTContext.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Range.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILOpenedArchetypesTracker.h"
#include "swift/SIL/SILVTable.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
using namespace swift;

// The verifier is basically all assertions, so don't compile it with NDEBUG to
// prevent release builds from triggering spurious unused variable warnings.
#ifndef NDEBUG

//===----------------------------------------------------------------------===//
//                    OwnershipCompatibilityCheckerVisitor
//===----------------------------------------------------------------------===//

namespace {

struct OwnershipUseCheckerResult {
  bool CompatibleOwnership;
  bool ShouldCheckForDataflowViolations;
};

class OwnershipCompatibilityUseChecker
    : public SILInstructionVisitor<OwnershipCompatibilityUseChecker,
                                   OwnershipUseCheckerResult> {
  ValueBase *Value;
  unsigned OpIndex;
  ValueOwnershipKind OwnershipKind;
  llvm::SmallVector<SILInstruction *, 8> UsesToCheck;

public:
  OwnershipCompatibilityUseChecker(const ValueBase *V)
      : Value(const_cast<ValueBase *>(V)),
        OwnershipKind(Value->getOwnershipKind()), UsesToCheck() {}

  ArrayRef<SILInstruction *> getUsesToCheck() const { return UsesToCheck; }

  void error(SILInstruction *User) {
    llvm::errs() << "Have operand with incompatible ownership?!\n"
                 << "Value: " << *Value << "User: " << *User
                 << "Conv: " << OwnershipKind << "\n";
    llvm_unreachable("triggering standard assertion failure routine");
  }

  void check() {
    // Bail early if we do not have owned ownership. This is temporary and just
    // for testing purposes.
    if (OwnershipKind != ValueOwnershipKind::Owned)
      return;

    for (const Operand *Op : Value->getUses()) {
      auto *User = const_cast<SILInstruction *>(Op->getUser());
      OpIndex = Op->getOperandNumber();
      auto Result = visit(User);
      if (!Result.CompatibleOwnership) {
        error(User);
      }
      if (Result.ShouldCheckForDataflowViolations) {
        UsesToCheck.push_back(User);
      }
    }
  }

  OwnershipUseCheckerResult visitValueBase(ValueBase *) {
    llvm_unreachable("Unimplemented?!");
  }
// Create declarations for all instructions, so we get a warning at compile
// time if any instructions do not have an implementation.
#define INST(Id, Parent, TextualName, MemBehavior, MayRelease)                 \
  OwnershipUseCheckerResult visit##Id(Id *);
#include "swift/SIL/SILNodes.def"
};

} // end anonymous namespace

/// Implementation for instructions without operands. These should never be
/// visited.
#define NO_OPERAND_INST(INST)                                                  \
  OwnershipUseCheckerResult                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() == 0 &&                                         \
           "Expected instruction without operands?!");                         \
    llvm_unreachable("Instruction without operand can not be compatible with " \
                     "any def's OwnershipValueKind");                          \
  }
NO_OPERAND_INST(AllocStack)
NO_OPERAND_INST(AllocRef)
NO_OPERAND_INST(AllocRefDynamic)
NO_OPERAND_INST(AllocValueBuffer)
NO_OPERAND_INST(AllocBox)
NO_OPERAND_INST(AllocExistentialBox)
NO_OPERAND_INST(StrongRetain)
NO_OPERAND_INST(StrongRetainUnowned)
NO_OPERAND_INST(UnownedRetain)
NO_OPERAND_INST(RetainValue)
NO_OPERAND_INST(AllocGlobal)
NO_OPERAND_INST(FunctionRef)
NO_OPERAND_INST(GlobalAddr)
NO_OPERAND_INST(IntegerLiteral)
NO_OPERAND_INST(FloatLiteral)
NO_OPERAND_INST(StringLiteral)
NO_OPERAND_INST(Metatype)
NO_OPERAND_INST(Unreachable)
#undef NO_OPERAND_INST

/// Instructions whose arguments are always compatible with one convention.
#define CONSTANT_OWNERSHIP_INST(INST, OWNERSHIP,                               \
                                SHOULD_CHECK_FOR_DATAFLOW_VIOLATIONS)          \
  OwnershipUseCheckerResult                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() && "Expected to have non-zero operands");       \
    auto Result =                                                              \
        ValueOwnershipKindMerge(ValueOwnershipKind::OWNERSHIP, OwnershipKind); \
    return {Result.hasValue(), SHOULD_CHECK_FOR_DATAFLOW_VIOLATIONS};          \
  }
CONSTANT_OWNERSHIP_INST(DeallocStack, Any, false)
CONSTANT_OWNERSHIP_INST(DeallocRef, Any, false)
CONSTANT_OWNERSHIP_INST(DeallocPartialRef, Any, false)
CONSTANT_OWNERSHIP_INST(DeallocValueBuffer, Any, false)
CONSTANT_OWNERSHIP_INST(DeallocBox, Any, false)
CONSTANT_OWNERSHIP_INST(DeallocExistentialBox, Any, false)
CONSTANT_OWNERSHIP_INST(Load, Any, false)
CONSTANT_OWNERSHIP_INST(LoadBorrow, Any, false)
CONSTANT_OWNERSHIP_INST(BeginBorrow, Any, false)
CONSTANT_OWNERSHIP_INST(LoadWeak, Any, false)
CONSTANT_OWNERSHIP_INST(StoreBorrow, Any, false)
CONSTANT_OWNERSHIP_INST(LoadUnowned, Any, false)
CONSTANT_OWNERSHIP_INST(MarkUninitialized, Any, false)
CONSTANT_OWNERSHIP_INST(MarkUninitializedBehavior, Any, false)
CONSTANT_OWNERSHIP_INST(MarkFunctionEscape, Any, false)
CONSTANT_OWNERSHIP_INST(DebugValue, Any, false)
CONSTANT_OWNERSHIP_INST(DebugValueAddr, Any, false)
CONSTANT_OWNERSHIP_INST(CopyAddr, Any, false)
CONSTANT_OWNERSHIP_INST(DestroyAddr, Any, false)
CONSTANT_OWNERSHIP_INST(ProjectValueBuffer, Any, false)
CONSTANT_OWNERSHIP_INST(ProjectBox, Any, false)
CONSTANT_OWNERSHIP_INST(ProjectExistentialBox, Any, false)
CONSTANT_OWNERSHIP_INST(IndexAddr, Any, false)
CONSTANT_OWNERSHIP_INST(TailAddr, Any, false)
CONSTANT_OWNERSHIP_INST(IndexRawPointer, Any, false)
CONSTANT_OWNERSHIP_INST(BindMemory, Any, false)
CONSTANT_OWNERSHIP_INST(StrongPin, Any, false)
CONSTANT_OWNERSHIP_INST(StrongUnpin, Owned, true)
CONSTANT_OWNERSHIP_INST(StrongRelease, Owned, true)
CONSTANT_OWNERSHIP_INST(UnownedRelease, Owned, true)
CONSTANT_OWNERSHIP_INST(ReleaseValue, Owned, true)
CONSTANT_OWNERSHIP_INST(SetDeallocating, Any, false)
CONSTANT_OWNERSHIP_INST(AutoreleaseValue, Owned, true)
CONSTANT_OWNERSHIP_INST(FixLifetime, Any, false)
CONSTANT_OWNERSHIP_INST(MarkDependence, Any, false)
CONSTANT_OWNERSHIP_INST(CopyBlock, Any, false)
CONSTANT_OWNERSHIP_INST(CopyValue, Any, false)
CONSTANT_OWNERSHIP_INST(DestroyValue, Owned, true)
CONSTANT_OWNERSHIP_INST(IsUnique, Any, false)
CONSTANT_OWNERSHIP_INST(IsUniqueOrPinned, Any, false)
CONSTANT_OWNERSHIP_INST(ClassMethod, Any, false)
CONSTANT_OWNERSHIP_INST(SuperMethod, Any, false)
CONSTANT_OWNERSHIP_INST(WitnessMethod, Any, false)
CONSTANT_OWNERSHIP_INST(DynamicMethod, Any, false)
CONSTANT_OWNERSHIP_INST(ValueMetatype, Any, false)
CONSTANT_OWNERSHIP_INST(ExistentialMetatype, Any, false)
CONSTANT_OWNERSHIP_INST(ObjCProtocol, Any, false)
CONSTANT_OWNERSHIP_INST(ObjCMetatypeToObject, Any, false)
CONSTANT_OWNERSHIP_INST(ObjCExistentialMetatypeToObject, Any, false)
CONSTANT_OWNERSHIP_INST(ObjCToThickMetatype, Any, false)
CONSTANT_OWNERSHIP_INST(ThickToObjCMetatype, Any, false)
CONSTANT_OWNERSHIP_INST(ThinToThickFunction, Any, false)
CONSTANT_OWNERSHIP_INST(BridgeObjectToWord, Any, false)
CONSTANT_OWNERSHIP_INST(UncheckedTrivialBitCast, Any, false)
CONSTANT_OWNERSHIP_INST(UncheckedBitwiseCast, Any, false)
CONSTANT_OWNERSHIP_INST(RefToRawPointer, Any, false)
CONSTANT_OWNERSHIP_INST(RawPointerToRef, Trivial, false)
CONSTANT_OWNERSHIP_INST(RefToUnowned, Any, false)
CONSTANT_OWNERSHIP_INST(UnownedToRef, Any, false)
CONSTANT_OWNERSHIP_INST(RefToUnmanaged, Any, false)
CONSTANT_OWNERSHIP_INST(UnmanagedToRef, Any, false)
CONSTANT_OWNERSHIP_INST(ThinFunctionToPointer, Any, false)
CONSTANT_OWNERSHIP_INST(PointerToThinFunction, Any, false)
CONSTANT_OWNERSHIP_INST(IsNonnull, Any, false)
CONSTANT_OWNERSHIP_INST(CondFail, Any, false)
CONSTANT_OWNERSHIP_INST(SwitchValue, Any, false)
CONSTANT_OWNERSHIP_INST(SwitchEnum, Owned, true)
CONSTANT_OWNERSHIP_INST(SwitchEnumAddr, Any, false)
CONSTANT_OWNERSHIP_INST(DynamicMethodBranch, Any, false)
CONSTANT_OWNERSHIP_INST(CheckedCastAddrBranch, Any, false)
CONSTANT_OWNERSHIP_INST(UnconditionalCheckedCastAddr, Any, false)
CONSTANT_OWNERSHIP_INST(UncheckedRefCastAddr, Any, false)
CONSTANT_OWNERSHIP_INST(UncheckedAddrCast, Any, false)
CONSTANT_OWNERSHIP_INST(AddressToPointer, Any, false)
CONSTANT_OWNERSHIP_INST(PointerToAddress, Any, false)
CONSTANT_OWNERSHIP_INST(ProjectBlockStorage, Any, false)
CONSTANT_OWNERSHIP_INST(InitBlockStorageHeader, Any, false)
CONSTANT_OWNERSHIP_INST(InitExistentialMetatype, Any, false)
CONSTANT_OWNERSHIP_INST(OpenExistentialMetatype, Any, false)
CONSTANT_OWNERSHIP_INST(OpenExistentialBox, Any, false)
CONSTANT_OWNERSHIP_INST(UncheckedTakeEnumDataAddr, Any, false)
CONSTANT_OWNERSHIP_INST(InjectEnumAddr, Any, false)
CONSTANT_OWNERSHIP_INST(SelectEnum, Any, false)
CONSTANT_OWNERSHIP_INST(SelectEnumAddr, Any, false)
CONSTANT_OWNERSHIP_INST(SelectValue, Any, false)
CONSTANT_OWNERSHIP_INST(InitExistentialAddr, Any, false)
CONSTANT_OWNERSHIP_INST(DeinitExistentialAddr, Any, false)
CONSTANT_OWNERSHIP_INST(OpenExistentialAddr, Any, false)
CONSTANT_OWNERSHIP_INST(InitEnumDataAddr, Any, false)
CONSTANT_OWNERSHIP_INST(TupleElementAddr, Any, false)
CONSTANT_OWNERSHIP_INST(StructElementAddr, Any, false)
CONSTANT_OWNERSHIP_INST(RefElementAddr, Any, false)
CONSTANT_OWNERSHIP_INST(RefTailAddr, Any, false)
#undef CONSTANT_OWNERSHIP_INST

#define FORWARD_OWNERSHIP_INST(INST)                                           \
  OwnershipUseCheckerResult                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() && "Expected to have non-zero operands");       \
    ArrayRef<Operand> Ops = I->getAllOperands();                               \
    llvm::Optional<ValueOwnershipKind> Base = OwnershipKind;                   \
    for (const Operand &Op : Ops) {                                            \
      Base = ValueOwnershipKindMerge(Base, Op.get()->getOwnershipKind());      \
    }                                                                          \
    return {Base.hasValue(), true};                                            \
  }

FORWARD_OWNERSHIP_INST(Tuple)
FORWARD_OWNERSHIP_INST(TupleExtract)
FORWARD_OWNERSHIP_INST(Struct)
FORWARD_OWNERSHIP_INST(StructExtract)
FORWARD_OWNERSHIP_INST(Enum)
FORWARD_OWNERSHIP_INST(UncheckedEnumData)
FORWARD_OWNERSHIP_INST(InitExistentialRef)
FORWARD_OWNERSHIP_INST(OpenExistentialRef)
FORWARD_OWNERSHIP_INST(Upcast)
FORWARD_OWNERSHIP_INST(UncheckedRefCast)
FORWARD_OWNERSHIP_INST(ConvertFunction)
FORWARD_OWNERSHIP_INST(RefToBridgeObject)
FORWARD_OWNERSHIP_INST(BridgeObjectToRef)
FORWARD_OWNERSHIP_INST(UnconditionalCheckedCast)
FORWARD_OWNERSHIP_INST(Branch)
FORWARD_OWNERSHIP_INST(CondBranch)
FORWARD_OWNERSHIP_INST(CheckedCastBranch)
#undef FORWARD_OWNERSHIP_INST

static bool compatibleOwnershipKinds(ValueOwnershipKind K1,
                                     ValueOwnershipKind K2) {
  return ValueOwnershipKindMerge(K1, K2).hasValue();
}

static ValueOwnershipKind
compatibleOwnershipForResultConvention(ResultConvention C) {
  switch (C) {
  case ResultConvention::Indirect:
    llvm_unreachable("Indirect can never be result values");
  case ResultConvention::Autoreleased:
  case ResultConvention::Owned:
    return ValueOwnershipKind::Owned;
  case ResultConvention::Unowned:
  case ResultConvention::UnownedInnerPointer:
    return ValueOwnershipKind::Unowned;
  }
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitReturnInst(ReturnInst *I) {
  assert(I->getNumOperands() &&
         "Return without operand that is a use of a def?!");
  SILResultInfo RI = I->getFunction()->getSingleResult();
  auto Kind = compatibleOwnershipForResultConvention(RI.getConvention());
  auto Result = ValueOwnershipKindMerge(Kind, OwnershipKind);
  return {compatibleOwnershipKinds(Kind, OwnershipKind), true};
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitEndBorrowInst(EndBorrowInst *I) {
  // We do not consider the source to be a verified use for now.
  if (OpIndex == EndBorrowInst::Src)
    return {true, false};
  return {
      compatibleOwnershipKinds(ValueOwnershipKind::Guaranteed, OwnershipKind),
      false};
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitThrowInst(ThrowInst *I) {
  // Error objects are trivial? If this fails, fix this.
  return {true, false};
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitStoreUnownedInst(StoreUnownedInst *I) {
  if (Value == I->getSrc())
    return {compatibleOwnershipKinds(OwnershipKind, ValueOwnershipKind::Owned),
            true};
  return {true, false};
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitStoreWeakInst(StoreWeakInst *I) {
  if (Value == I->getSrc())
    return {compatibleOwnershipKinds(ValueOwnershipKind::Owned, OwnershipKind),
            true};
  return {true, false};
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitApplyInst(ApplyInst *I) {
  // The first operand of an apply is the callee.
  switch (I->getArgumentConvention(OpIndex - 1)) {
  case SILArgumentConvention::Indirect_In:
  case SILArgumentConvention::Indirect_In_Guaranteed:
  case SILArgumentConvention::Indirect_Inout:
  case SILArgumentConvention::Indirect_InoutAliasable:
  case SILArgumentConvention::Indirect_Out:
    return {true, false};
  case SILArgumentConvention::Direct_Owned:
    return {ValueOwnershipKind::Owned == OwnershipKind, true};
  case SILArgumentConvention::Direct_Unowned:
    if (Value->getType().isTrivial(I->getModule()))
      return {
          compatibleOwnershipKinds(ValueOwnershipKind::Trivial, OwnershipKind),
          false};
    return {
        compatibleOwnershipKinds(ValueOwnershipKind::Unowned, OwnershipKind),
        false};
  case SILArgumentConvention::Direct_Guaranteed:
    return {
        compatibleOwnershipKinds(ValueOwnershipKind::Guaranteed, OwnershipKind),
        false};
  case SILArgumentConvention::Direct_Deallocating:
    llvm_unreachable("No ownership associated with deallocating");
  }
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitTryApplyInst(TryApplyInst *I) {
  // The first operand of an apply is the callee.
  switch (I->getArgumentConvention(OpIndex - 1)) {
  case SILArgumentConvention::Indirect_In:
  case SILArgumentConvention::Indirect_In_Guaranteed:
  case SILArgumentConvention::Indirect_Inout:
  case SILArgumentConvention::Indirect_InoutAliasable:
  case SILArgumentConvention::Indirect_Out:
    return {true, false};
  case SILArgumentConvention::Direct_Owned:
    return {compatibleOwnershipKinds(ValueOwnershipKind::Owned, OwnershipKind),
            true};
  case SILArgumentConvention::Direct_Unowned:
    if (Value->getType().isTrivial(I->getModule()))
      return {
          compatibleOwnershipKinds(ValueOwnershipKind::Trivial, OwnershipKind),
          false};
    return {
        compatibleOwnershipKinds(ValueOwnershipKind::Unowned, OwnershipKind),
        false};
  case SILArgumentConvention::Direct_Guaranteed:
    return {
        compatibleOwnershipKinds(ValueOwnershipKind::Guaranteed, OwnershipKind),
        false};
  case SILArgumentConvention::Direct_Deallocating:
    llvm_unreachable("No ownership associated with deallocating");
  }
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitPartialApplyInst(PartialApplyInst *I) {
  // All non-trivial types should be captured.
  if (Value->getType().isTrivial(I->getModule())) {
    return {
        compatibleOwnershipKinds(ValueOwnershipKind::Trivial, OwnershipKind),
        false};
  }
  return {compatibleOwnershipKinds(ValueOwnershipKind::Owned, OwnershipKind),
          true};
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitBuiltinInst(BuiltinInst *I) {
  // This needs to be updated later.
  return {true, false};
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitAssignInst(AssignInst *I) {
  if (Value == I->getSrc())
    return {compatibleOwnershipKinds(ValueOwnershipKind::Owned, OwnershipKind),
            true};
  return {true, false};
}

OwnershipUseCheckerResult
OwnershipCompatibilityUseChecker::visitStoreInst(StoreInst *I) {
  if (Value == I->getSrc())
    return {compatibleOwnershipKinds(ValueOwnershipKind::Owned, OwnershipKind),
            true};
  return {true, false};
}

//===----------------------------------------------------------------------===//
//                         ValueBaseOwnershipChecker
//===----------------------------------------------------------------------===//

/*

The Ownership Checker Algorithm
===============================

This portion of the verifier implements the SIL ownership checker
algorithm. Let V be the value that we are trying to check. Then:

1. Take all uses of V and verifier that none of them are in the same block. Then
all of these blocks to a Worklist.

2. Until the Worklist is empty,

*/
namespace {

class ValueBaseOwnershipChecker {
  /// The value whose ownership we will check.
  const ValueBase *Value;

#ifndef NDEBUG
  llvm::SmallVector<const SILInstruction *, 8> UsesToCheck;
#endif

  /// The worklist that we use for our computation.
  llvm::SmallVector<const SILBasicBlock *, 32> Worklist;

  /// Blocks that contain uses of the SILValue. If we ever visit one of these
  /// twice, we have an over-consume.
  llvm::SmallPtrSet<const SILBasicBlock *, 8> BlocksWithUses;

  /// The blocks that we have visited as part of our traversal. We use this to
  /// ensure we only visit a block once.
  llvm::SmallPtrSet<const SILBasicBlock *, 32> VisitedBlocks;

  /// Successors of blocks that we have visited. When we have finished our
  /// traversal, this should be empty, otherwise the consumers do not joint
  /// post-dominate the producer of the value.
  llvm::SmallPtrSet<const SILBasicBlock *, 32> MustVisitBlocks;

public:
  ValueBaseOwnershipChecker(const ValueBase *V) : Value(V) {}

  ~ValueBaseOwnershipChecker() = default;
  ValueBaseOwnershipChecker(const ValueBaseOwnershipChecker &) = delete;
  ValueBaseOwnershipChecker(ValueBaseOwnershipChecker &&) = delete;

#ifndef NDEBUG
  ArrayRef<const SILInstruction *> getUsesToCheck() const {
    return UsesToCheck;
  }
#endif

  void check() {
    // First check that our uses have coherent ownership. If after evaluating
    // the ownership we do not need to check dataflow (due to performs
    // ValueOwnershipKind::None), then bail.
    if (!checkUses())
      return;
#ifndef NDEBUG
    ArrayRef<const SILInstruction *> Uses = getUsesToCheck();
#endif
    checkDataflow();
  }

private:
  bool checkUses();
  void checkDataflow();
};

} // end anonymous namespace

bool ValueBaseOwnershipChecker::checkUses() {
  // First make sure that V does not have None ownership. If it has None
  // ownership, then there is nothing to check.
  auto Ownership = Value->getOwnershipKind();

  // As an initial experiment, see what happens with this.
  // if (Ownership == ValueOwnershipKind::None)
  if (Ownership != ValueOwnershipKind::Owned)
    return false;

  // First go through V and gather up its uses. While we do this we:
  //
  // 1. Verify that none of the uses are in the same block. This would be an
  // overconsume so in this case we assert.
  // 2. Verify that the uses are compatible with our ownership convention.
  OwnershipCompatibilityUseChecker CompatibilityChecker(Value);
  CompatibilityChecker.check();

  for (const SILInstruction *User : CompatibilityChecker.getUsesToCheck()) {
#ifndef NDEBUG
    UsesToCheck.push_back(User);
#endif
    const SILBasicBlock *UserBlock = User->getParent();
    bool InsertedBlock = BlocksWithUses.insert(UserBlock).second;
    if (!InsertedBlock) {
      llvm::errs() << "Found over consume?!\n"
                   << "Value: " << *Value << "User: " << *User << "Block:\n"
                   << *UserBlock << "\n";
      llvm_unreachable("triggering standard assertion failure routine");
    }

    // Add the user block to the visited list so we do not try to add it to our
    // must visit successor list.
    VisitedBlocks.insert(UserBlock);

    // Now add all predecessors of this block to the worklist.
    for (const SILBasicBlock *Preds : UserBlock->getPreds()) {
      Worklist.push_back(Preds);
    }
  }

  return true;
}

void ValueBaseOwnershipChecker::checkDataflow() {
  // Until the worklist is empty...
  while (!Worklist.empty()) {
    // Grab the next block to visit.
    const SILBasicBlock *BB = Worklist.pop_back_val();

    // Add it to the visit list. If we have already visited this block,
    // continue.
    if (!VisitedBlocks.insert(BB).second) {
      continue;
    }

    // If this block was a successor that we needed to visit to make sure we
    // joint post-dominated, remove it.
    MustVisitBlocks.erase(BB);

    // Then assert that this block is not a use containing block. If we are
    // visiting a use-containing block, we have an over-consume.
    bool IsBlockWithUses = BlocksWithUses.count(BB);
    if (IsBlockWithUses) {
      llvm::errs() << "Found over consume?!\n"
                   << "Value: " << *Value << "Block:\n"
                   << *BB << "\n";
      llvm_unreachable("triggering standard assertion failure routine");
    }

    // Ok, now we know that we do not have an overconsume. So now we need to
    // update our state for our successors to make sure by the end of the block,
    // we visit them.
    for (const SILBasicBlock *SuccBlock : BB->getSuccessorBlocks()) {
      // If we already visited the successor, there is nothing to do since we
      // already visited the successor.
      if (VisitedBlocks.count(SuccBlock))
        continue;

      // Otherwise, add the successor to our MustVisitBlocks set to ensure that
      // we assert if we do not visit it by the end of the algorithm.
      MustVisitBlocks.insert(SuccBlock);
    }

    // Then add each predecessor of this block to the worklist if we have not
    // visited it yet.
    for (const SILBasicBlock *PredBlock : BB->getPreds()) {
      if (VisitedBlocks.count(PredBlock))
        continue;
      Worklist.push_back(PredBlock);
    }
  }

  if (MustVisitBlocks.empty())
    return;

  llvm::errs() << "Error! Found consuming post-dominance failure!\n"
               << "    Value: " << *Value
               << "    Post Dominating Failure Blocks:\n";
  for (auto *BB : MustVisitBlocks) {
    llvm::errs() << *BB;
  }
  llvm_unreachable("triggering standard assertion failure routine");
}
#endif

//===----------------------------------------------------------------------===//
//                           Top Level Entrypoints
//===----------------------------------------------------------------------===//

void ValueBase::verifyOwnership() const {
#ifndef NDEBUG
  ValueBaseOwnershipChecker(this).check();
#endif
}

void SILInstruction::verifyOperandOwnership() const {
#ifndef NDEBUG
  for (const Operand &Op : getAllOperands()) {
    OwnershipCompatibilityUseChecker(Op.get()).check();
  }
#endif
}
