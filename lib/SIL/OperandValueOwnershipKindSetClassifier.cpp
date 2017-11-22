//===--- OperandValueOwnershipKindSetClassifier.cpp -----------------------===//
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
///
/// \file
///
/// This file implements a classifier that maps an operand of an instruction
/// (i.e. a use) to the OptionSet of ValueOwnershipKinds that it can accept.
///
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILValue.h"

using namespace swift;

class OperandValueOwnershipKindSetClassifier
    : public SILInstructionVisitor<OperandValueOwnershipKindSetClassifier,
                                   OperandValueOwnershipKindSet> {
public:
  SILModule &Mod;
  const Operand &Op;

public:
  OperandValueOwnershipKindSetClassifier(SILModule &M, const Operand &Op) {
    : Mod(M), Op(Op) {}

  SILValue getValue() const { return Op.get(); }

  ValueOwnershipKind getOwnershipKind() const {
    assert(getValue().getOwnershipKind() == Op.get().getOwnershipKind() &&
           "Expected ownership kind of parent value and operand");
    return getValue().getOwnershipKind();
  }

  unsigned getOperandIndex() const { return Op.getOperandNumber(); }

  SILType getType() const { return Op.get()->getType(); }

  bool compatibleWithOwnership(ValueOwnershipKind Kind) const {
    return compatibleOwnershipKinds(getOwnershipKind(), Kind);
  }

  bool hasExactOwnership(ValueOwnershipKind Kind) const {
    return getOwnershipKind() == Kind;
  }

  bool isAddressOrTrivialType() const {
    if (getType().isAddress())
      return true;
    return getOwnershipKind() == ValueOwnershipKind::Trivial ||
      getOwnershipKind() == ValueOwnershipKind::Any;
  }

  OperandValueOwnershipKindSet visitForwardingInst(SILInstruction *I,
                                                ArrayRef<Operand> Ops);
  OperandValueOwnershipKindSet visitForwardingInst(SILInstruction *I) {
    return visitForwardingInst(I, I->getAllOperands());
  }

  /// Visit a terminator instance that performs a transform like
  /// operation. E.x.: switch_enum, checked_cast_br. This does not include br or
  /// cond_br.
  OperandValueOwnershipKindSet visitTransformingTerminatorInst(TermInst *TI);

  OperandValueOwnershipKindSet
  visitEnumArgument(EnumDecl *E, ValueOwnershipKind RequiredConvention);
  OperandValueOwnershipKindSet
  visitApplyParameter(ValueOwnershipKind RequiredConvention,
                      UseLifetimeConstraint Requirement);
  OperandValueOwnershipKindSet
  visitFullApply(FullApplySite apply);

  OperandValueOwnershipKindSet visitCallee(CanSILFunctionType SubstCalleeType);
  OperandValueOwnershipKindSet
  checkTerminatorArgumentMatchesDestBB(SILBasicBlock *DestBB, unsigned OpIndex);

// Create declarations for all instructions, so we get a warning at compile
// time if any instructions do not have an implementation.
#define INST(Id, Parent) \
  OperandValueOwnershipKindSet visit##Id(Id *);
#include "swift/SIL/SILNodes.def"
};

} // end anonymous namespace

/// Implementation for instructions without operands. These should never be
/// visited.
#define NO_OPERAND_INST(INST)                                                  \
  OperandValueOwnershipKindSet                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() == 0 &&                                         \
           "Expected instruction without operands?!");                         \
    llvm_unreachable("Instruction without operand can not be compatible with " \
                     "any def's OwnershipValueKind");                          \
  }
NO_OPERAND_INST(AllocBox)
NO_OPERAND_INST(AllocExistentialBox)
NO_OPERAND_INST(AllocGlobal)
NO_OPERAND_INST(AllocStack)
NO_OPERAND_INST(FloatLiteral)
NO_OPERAND_INST(FunctionRef)
NO_OPERAND_INST(GlobalAddr)
NO_OPERAND_INST(GlobalValue)
NO_OPERAND_INST(IntegerLiteral)
NO_OPERAND_INST(Metatype)
NO_OPERAND_INST(ObjCProtocol)
NO_OPERAND_INST(RetainValue)
NO_OPERAND_INST(RetainValueAddr)
NO_OPERAND_INST(StringLiteral)
NO_OPERAND_INST(ConstStringLiteral)
NO_OPERAND_INST(StrongRetain)
NO_OPERAND_INST(StrongRetainUnowned)
NO_OPERAND_INST(UnownedRetain)
NO_OPERAND_INST(Unreachable)
NO_OPERAND_INST(Unwind)
#undef NO_OPERAND_INST

/// Instructions whose arguments are always compatible with one convention.
#define CONSTANT_OWNERSHIP_INST(OWNERSHIP, USE_LIFETIME_CONSTRAINT, INST)      \
  OperandValueOwnershipKindSet                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() && "Expected to have non-zero operands");       \
    if (ValueOwnershipKind::OWNERSHIP == ValueOwnershipKind::Trivial) {        \
      assert(isAddressOrTrivialType() &&                                       \
             "Trivial ownership requires a trivial type or an address");       \
    }                                                                          \
                                                                               \
    return {{ValueOwnershipKind::OWNERSHIP},                            \
            UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT};                   \
  }
CONSTANT_OWNERSHIP_INST(Guaranteed, MustBeLive, RefElementAddr)
CONSTANT_OWNERSHIP_INST(Guaranteed, MustBeLive, OpenExistentialValue)
CONSTANT_OWNERSHIP_INST(Guaranteed, MustBeLive, OpenExistentialBoxValue)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, AutoreleaseValue)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, DeallocBox)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, DeallocExistentialBox)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, DeallocRef)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, DestroyValue)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, ReleaseValue)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, ReleaseValueAddr)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, StrongRelease)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, StrongUnpin)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, UnownedRelease)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, InitExistentialRef)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, EndLifetime)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, AbortApply)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, AddressToPointer)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, BeginAccess)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, BeginUnpairedAccess)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, BindMemory)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, CheckedCastAddrBranch)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, CondFail)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, CopyAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, DeallocStack)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, DebugValueAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, DeinitExistentialAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, DestroyAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, EndAccess)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, EndApply)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, EndUnpairedAccess)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, IndexAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, IndexRawPointer)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, InitBlockStorageHeader)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, InitEnumDataAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, InitExistentialAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, InitExistentialMetatype)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, InjectEnumAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, IsUnique)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, IsUniqueOrPinned)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, Load)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, LoadBorrow)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, LoadUnowned)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, LoadWeak)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, MarkFunctionEscape)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, MarkUninitializedBehavior)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, ObjCExistentialMetatypeToObject)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, ObjCMetatypeToObject)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, ObjCToThickMetatype)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, OpenExistentialAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, OpenExistentialMetatype)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, PointerToAddress)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, PointerToThinFunction)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, ProjectBlockStorage)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, ProjectValueBuffer)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, RawPointerToRef)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, SelectEnumAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, SelectValue)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, StructElementAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, SwitchEnumAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, SwitchValue)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, TailAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, ThickToObjCMetatype)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, ThinFunctionToPointer)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, ThinToThickFunction)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, TupleElementAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, UncheckedAddrCast)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, UncheckedRefCastAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, UncheckedTakeEnumDataAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, UnconditionalCheckedCastAddr)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, UnmanagedToRef)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, AllocValueBuffer)
CONSTANT_OWNERSHIP_INST(Trivial, MustBeLive, DeallocValueBuffer)
#undef CONSTANT_OWNERSHIP_INST

/// Instructions whose arguments are always compatible with one convention.
#define CONSTANT_OR_TRIVIAL_OWNERSHIP_INST(OWNERSHIP, USE_LIFETIME_CONSTRAINT, \
                                           INST)                               \
  OperandValueOwnershipKindSet                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() && "Expected to have non-zero operands");       \
    if (ValueOwnershipKind::OWNERSHIP == ValueOwnershipKind::Trivial) {        \
      assert(isAddressOrTrivialType() &&                                       \
             "Trivial ownership requires a trivial type or an address");       \
    }                                                                          \
                                                                               \
    OperandValueOwnershipKindSet Result; \
    if (compatibleWithOwnership(ValueOwnershipKind::Trivial)) {                \
      return {ValueOwnershipKind::Trivial, UseLifetimeConstraint::MustBeLive}; \
    }                                                                          \
    return {compatibleWithOwnership(ValueOwnershipKind::OWNERSHIP),            \
            UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT};                   \
  }
CONSTANT_OR_TRIVIAL_OWNERSHIP_INST(Owned, MustBeInvalidated,
                                   CheckedCastValueBranch)
CONSTANT_OR_TRIVIAL_OWNERSHIP_INST(Owned, MustBeInvalidated,
                                   UnconditionalCheckedCastValue)
CONSTANT_OR_TRIVIAL_OWNERSHIP_INST(Owned, MustBeInvalidated,
                                   InitExistentialValue)
CONSTANT_OR_TRIVIAL_OWNERSHIP_INST(Owned, MustBeInvalidated,
                                   DeinitExistentialValue)
#undef CONSTANT_OR_TRIVIAL_OWNERSHIP_INST

#define ACCEPTS_ANY_OWNERSHIP_INST(INST)                                       \
  OperandValueOwnershipKindSet                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    return {true, UseLifetimeConstraint::MustBeLive};                          \
  }
ACCEPTS_ANY_OWNERSHIP_INST(BeginBorrow)
ACCEPTS_ANY_OWNERSHIP_INST(CopyValue)
ACCEPTS_ANY_OWNERSHIP_INST(DebugValue)
ACCEPTS_ANY_OWNERSHIP_INST(FixLifetime)
ACCEPTS_ANY_OWNERSHIP_INST(UncheckedBitwiseCast) // Is this right?
ACCEPTS_ANY_OWNERSHIP_INST(WitnessMethod)        // Is this right?
ACCEPTS_ANY_OWNERSHIP_INST(ProjectBox)           // The result is a T*.
ACCEPTS_ANY_OWNERSHIP_INST(DynamicMethodBranch)
ACCEPTS_ANY_OWNERSHIP_INST(UncheckedTrivialBitCast)
ACCEPTS_ANY_OWNERSHIP_INST(ExistentialMetatype)
ACCEPTS_ANY_OWNERSHIP_INST(ValueMetatype)
ACCEPTS_ANY_OWNERSHIP_INST(UncheckedOwnershipConversion)
#undef ACCEPTS_ANY_OWNERSHIP_INST

// Trivial if trivial typed, otherwise must accept owned?
#define ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP_OR_METATYPE(USE_LIFETIME_CONSTRAINT,  \
                                                     INST)                     \
  OperandValueOwnershipKindSet                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() && "Expected to have non-zero operands");       \
    if (getType().is<AnyMetatypeType>()) {                                     \
      return {true, UseLifetimeConstraint::MustBeLive};                        \
    }                                                                          \
    bool compatible = hasExactOwnership(ValueOwnershipKind::Any) ||            \
                      !compatibleWithOwnership(ValueOwnershipKind::Trivial);   \
    return {compatible, UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT};       \
  }
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP_OR_METATYPE(MustBeLive, ClassMethod)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP_OR_METATYPE(MustBeLive, ObjCMethod)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP_OR_METATYPE(MustBeLive, ObjCSuperMethod)
#undef ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP_OR_METATYPE

// Trivial if trivial typed, otherwise must accept owned?
#define ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(USE_LIFETIME_CONSTRAINT, INST)        \
  OperandValueOwnershipKindSet                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() && "Expected to have non-zero operands");       \
    bool compatible = hasExactOwnership(ValueOwnershipKind::Any) ||            \
                      !compatibleWithOwnership(ValueOwnershipKind::Trivial);   \
    return {compatible, UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT};       \
  }
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, SuperMethod)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, BridgeObjectToWord)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, CopyBlock)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, OpenExistentialBox)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, RefTailAddr)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, RefToRawPointer)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, RefToUnmanaged)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, RefToUnowned)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, SetDeallocating)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, StrongPin)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, UnownedToRef)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, CopyUnownedValue)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, ProjectExistentialBox)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, UnmanagedRetainValue)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, UnmanagedReleaseValue)
ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP(MustBeLive, UnmanagedAutoreleaseValue)
#undef ACCEPTS_ANY_NONTRIVIAL_OWNERSHIP

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitForwardingInst(SILInstruction *I, ArrayRef<Operand> Ops) {
  assert(I->getNumOperands() && "Expected to have non-zero operands");
  assert(isOwnershipForwardingInst(I) &&
         "Expected to have an ownership forwarding inst");

  // Find the first index where we have a trivial value.
  auto Iter = find_if(Ops, [&I](const Operand &Op) -> bool {
    if (I->isTypeDependentOperand(Op))
      return false;
    return Op.get().getOwnershipKind() != ValueOwnershipKind::Trivial;
  });

  // All trivial.
  if (Iter == Ops.end()) {
    return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
            UseLifetimeConstraint::MustBeLive};
  }

  unsigned Index = std::distance(Ops.begin(), Iter);
  ValueOwnershipKind Base = Ops[Index].get().getOwnershipKind();

  for (const Operand &Op : Ops.slice(Index + 1)) {
    if (I->isTypeDependentOperand(Op))
      continue;
    auto OpKind = Op.get().getOwnershipKind();
    if (OpKind.merge(ValueOwnershipKind::Trivial))
      continue;

    auto MergedValue = Base.merge(OpKind.Value);
    if (!MergedValue.hasValue()) {
      return {false, UseLifetimeConstraint::MustBeInvalidated};
    }
    Base = MergedValue.getValue();
  }

  // We only need to treat a forwarded instruction as a lifetime ending use of
  // it is owned.
  auto lifetimeConstraint = hasExactOwnership(ValueOwnershipKind::Owned)
                                ? UseLifetimeConstraint::MustBeInvalidated
                                : UseLifetimeConstraint::MustBeLive;
  return {true, lifetimeConstraint};
}

#define FORWARD_ANY_OWNERSHIP_INST(INST)                                       \
  OperandValueOwnershipKindSet                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    return visitForwardingInst(I);                                             \
  }
FORWARD_ANY_OWNERSHIP_INST(Tuple)
FORWARD_ANY_OWNERSHIP_INST(Struct)
FORWARD_ANY_OWNERSHIP_INST(Object)
FORWARD_ANY_OWNERSHIP_INST(Enum)
FORWARD_ANY_OWNERSHIP_INST(OpenExistentialRef)
FORWARD_ANY_OWNERSHIP_INST(Upcast)
FORWARD_ANY_OWNERSHIP_INST(UncheckedRefCast)
FORWARD_ANY_OWNERSHIP_INST(ConvertFunction)
FORWARD_ANY_OWNERSHIP_INST(RefToBridgeObject)
FORWARD_ANY_OWNERSHIP_INST(BridgeObjectToRef)
FORWARD_ANY_OWNERSHIP_INST(UnconditionalCheckedCast)
FORWARD_ANY_OWNERSHIP_INST(MarkUninitialized)
FORWARD_ANY_OWNERSHIP_INST(UncheckedEnumData)
FORWARD_ANY_OWNERSHIP_INST(DestructureStruct)
FORWARD_ANY_OWNERSHIP_INST(DestructureTuple)
#undef FORWARD_ANY_OWNERSHIP_INST

// An instruction that forwards a constant ownership or trivial ownership.
#define FORWARD_CONSTANT_OR_TRIVIAL_OWNERSHIP_INST(                            \
    OWNERSHIP, USE_LIFETIME_CONSTRAINT, INST)                                  \
  OperandValueOwnershipKindSet                                                    \
      OwnershipCompatibilityUseChecker::visit##INST##Inst(INST##Inst *I) {     \
    assert(I->getNumOperands() && "Expected to have non-zero operands");       \
    assert(isGuaranteedForwardingInst(I) &&                                    \
           "Expected an ownership forwarding inst");                           \
    if (ValueOwnershipKind::OWNERSHIP != ValueOwnershipKind::Trivial &&        \
        hasExactOwnership(ValueOwnershipKind::Trivial)) {                      \
      assert(isAddressOrTrivialType() &&                                       \
             "Trivial ownership requires a trivial type or an address");       \
      return {true, UseLifetimeConstraint::MustBeLive};                        \
    }                                                                          \
    if (ValueOwnershipKind::OWNERSHIP == ValueOwnershipKind::Trivial) {        \
      assert(isAddressOrTrivialType() &&                                       \
             "Trivial ownership requires a trivial type or an address");       \
    }                                                                          \
                                                                               \
    return {compatibleWithOwnership(ValueOwnershipKind::OWNERSHIP),            \
            UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT};                   \
  }
FORWARD_CONSTANT_OR_TRIVIAL_OWNERSHIP_INST(Guaranteed, MustBeLive, TupleExtract)
FORWARD_CONSTANT_OR_TRIVIAL_OWNERSHIP_INST(Guaranteed, MustBeLive,
                                           StructExtract)
#undef CONSTANT_OR_TRIVIAL_OWNERSHIP_INST

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitDeallocPartialRefInst(
    DeallocPartialRefInst *I) {
  if (getValue() == I->getInstance()) {
    return {compatibleWithOwnership(ValueOwnershipKind::Owned),
            UseLifetimeConstraint::MustBeInvalidated};
  }

  return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
          UseLifetimeConstraint::MustBeLive};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitEndBorrowArgumentInst(
    EndBorrowArgumentInst *I) {
  // If we are currently checking an end_borrow_argument as a subobject, then we
  // treat this as just a use.
  if (isCheckingSubObject())
    return {true, UseLifetimeConstraint::MustBeLive};

  // Otherwise, we must be checking an actual argument. Make sure it is guaranteed!
  auto lifetimeConstraint = hasExactOwnership(ValueOwnershipKind::Guaranteed)
                                ? UseLifetimeConstraint::MustBeInvalidated
                                : UseLifetimeConstraint::MustBeLive;
  return {true, lifetimeConstraint};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitSelectEnumInst(SelectEnumInst *I) {
  if (getValue() == I->getEnumOperand()) {
    return {true, UseLifetimeConstraint::MustBeLive};
  }

  return visitForwardingInst(I, I->getAllOperands().drop_front());
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitAllocRefInst(AllocRefInst *I) {
  assert(I->getNumOperands() != 0
         && "If we reach this point, we must have a tail operand");
  return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
          UseLifetimeConstraint::MustBeLive};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitAllocRefDynamicInst(
    AllocRefDynamicInst *I) {
  assert(I->getNumOperands() != 0 &&
         "If we reach this point, we must have a tail operand");
  return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
          UseLifetimeConstraint::MustBeLive};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::checkTerminatorArgumentMatchesDestBB(
    SILBasicBlock *DestBB, unsigned OpIndex) {
  // Grab the ownership kind of the destination block.
  ValueOwnershipKind DestBlockArgOwnershipKind =
      DestBB->getArgument(OpIndex)->getOwnershipKind();

  // Then if we do not have an enum, make sure that the conventions match.
  EnumDecl *E = getType().getEnumOrBoundGenericEnum();
  if (!E) {
    bool matches = compatibleWithOwnership(DestBlockArgOwnershipKind);
    auto lifetimeConstraint = hasExactOwnership(ValueOwnershipKind::Owned)
                                  ? UseLifetimeConstraint::MustBeInvalidated
                                  : UseLifetimeConstraint::MustBeLive;
    return {matches, lifetimeConstraint};
  }

  return visitEnumArgument(E, DestBlockArgOwnershipKind);
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitBranchInst(BranchInst *BI) {
  return checkTerminatorArgumentMatchesDestBB(BI->getDestBB(),
                                              getOperandIndex());
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitCondBranchInst(CondBranchInst *CBI) {
  // If our conditional branch is the condition, it is trivial. Check that the
  // ownership kind is trivial.
  if (CBI->isConditionOperandIndex(getOperandIndex()))
    return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
            UseLifetimeConstraint::MustBeLive};

  // Otherwise, make sure that our operand matches the
  if (CBI->isTrueOperandIndex(getOperandIndex())) {
    unsigned TrueOffset = 1;
    return checkTerminatorArgumentMatchesDestBB(CBI->getTrueBB(),
                                                getOperandIndex() - TrueOffset);
  }

  assert(CBI->isFalseOperandIndex(getOperandIndex()) &&
         "If an operand is not the condition index or a true operand index, it "
         "must be a false operand index");
  unsigned FalseOffset = 1 + CBI->getTrueOperands().size();
  return checkTerminatorArgumentMatchesDestBB(CBI->getFalseBB(),
                                              getOperandIndex() - FalseOffset);
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitSwitchEnumInst(SwitchEnumInst *SEI) {
  return visitTransformingTerminatorInst(SEI);
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitCheckedCastBranchInst(
    CheckedCastBranchInst *SEI) {
  return visitTransformingTerminatorInst(SEI);
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitTransformingTerminatorInst(
    TermInst *TI) {
  // If our operand was trivial, return early.
  if (compatibleWithOwnership(ValueOwnershipKind::Trivial))
    return {true, UseLifetimeConstraint::MustBeLive};

  // Then we need to go through all of our destinations and make sure that if
  // they have a payload, the payload's convention matches our
  // convention.
  //
  // *NOTE* we assume that all of our types line up and are checked by the
  // normal verifier.
  for (auto *Succ : TI->getParent()->getSuccessorBlocks()) {
    // This must be a no-payload case... continue.
    if (Succ->args_size() == 0)
      continue;

    // If we have a trivial value or a value with ownership kind that matches
    // the switch_enum, then continue.
    auto OwnershipKind = Succ->getArgument(0)->getOwnershipKind();
    if (OwnershipKind == ValueOwnershipKind::Trivial ||
        compatibleWithOwnership(OwnershipKind))
      continue;

    // Otherwise, emit an error.
    handleError([&]() {
      llvm::errs()
          << "Function: '" << Succ->getParent()->getName() << "'\n"
          << "Error! Argument ownership kind does not match terminator!\n"
          << "Terminator: " << *TI << "Argument: " << *Succ->getArgument(0)
          << "Expected convention: " << getOwnershipKind() << ".\n"
          << "Actual convention:   " << OwnershipKind << '\n'
          << '\n';
    });
  }

  // Finally, if everything lines up, emit that we match and are a lifetime
  // ending point if we are owned.
  auto lifetimeConstraint = hasExactOwnership(ValueOwnershipKind::Owned)
                                ? UseLifetimeConstraint::MustBeInvalidated
                                : UseLifetimeConstraint::MustBeLive;
  return {true, lifetimeConstraint};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitReturnInst(ReturnInst *RI) {
  SILModule &M = RI->getModule();
  bool IsTrivial = RI->getOperand()->getType().isTrivial(M);
  SILFunctionConventions fnConv = RI->getFunction()->getConventions();
  auto Results = fnConv.getDirectSILResults();
  if (Results.empty() || IsTrivial) {
    return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
            UseLifetimeConstraint::MustBeLive};
  }

  CanGenericSignature Sig = fnConv.funcTy->getGenericSignature();

  // Find the first index where we have a trivial value.
  auto Iter = find_if(Results, [&M, &Sig](const SILResultInfo &Info) -> bool {
    return Info.getOwnershipKind(M, Sig) != ValueOwnershipKind::Trivial;
  });

  // If we have all trivial, then we must be trivial. Why wasn't our original
  // type trivial? This is a hard error since this is a logic error in our code
  // here.
  if (Iter == Results.end())
    llvm_unreachable("Should have already checked a trivial type?!");

  ValueOwnershipKind Base = Iter->getOwnershipKind(M, Sig);

  for (const SILResultInfo &ResultInfo :
       SILFunctionConventions::DirectSILResultRange(std::next(Iter),
                                                    Results.end())) {
    auto RKind = ResultInfo.getOwnershipKind(M, Sig);
    // Ignore trivial types.
    if (RKind.merge(ValueOwnershipKind::Trivial))
      continue;

    auto MergedValue = Base.merge(RKind);
    // If we fail to merge all types in, bail. We can not come up with a proper
    // result type.
    if (!MergedValue.hasValue()) {
      return {false, UseLifetimeConstraint::MustBeLive};
    }
    // In case Base is Any.
    Base = MergedValue.getValue();
  }

  if (auto *E = getType().getEnumOrBoundGenericEnum()) {
    return visitEnumArgument(E, Base);
  }

  return {compatibleWithOwnership(Base),
          UseLifetimeConstraint::MustBeInvalidated};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitEndBorrowInst(EndBorrowInst *I) {
  // We do not consider the original value to be a verified use. But the value
  // does need to be alive.
  if (getOperandIndex() == EndBorrowInst::OriginalValue)
    return {true, UseLifetimeConstraint::MustBeLive};
  // The borrowed value is a verified use though of the begin_borrow.
  return {compatibleWithOwnership(ValueOwnershipKind::Guaranteed),
          UseLifetimeConstraint::MustBeInvalidated};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitThrowInst(ThrowInst *I) {
  return {compatibleWithOwnership(ValueOwnershipKind::Owned),
          UseLifetimeConstraint::MustBeInvalidated};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitStoreUnownedInst(StoreUnownedInst *I) {
  if (getValue() == I->getSrc())
    return {compatibleWithOwnership(ValueOwnershipKind::Owned),
            UseLifetimeConstraint::MustBeInvalidated};
  return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
          UseLifetimeConstraint::MustBeLive};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitStoreWeakInst(StoreWeakInst *I) {
  // A store_weak instruction implies that the value to be stored to be live,
  // but it does not touch the strong reference count of the value.
  if (getValue() == I->getSrc())
    return {true, UseLifetimeConstraint::MustBeLive};
  return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
          UseLifetimeConstraint::MustBeLive};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitStoreBorrowInst(StoreBorrowInst *I) {
  if (getValue() == I->getSrc())
    return {compatibleWithOwnership(ValueOwnershipKind::Guaranteed),
            UseLifetimeConstraint::MustBeLive};
  return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
          UseLifetimeConstraint::MustBeLive};
}

// FIXME: Why not use SILArgumentConvention here?
OperandValueOwnershipKindSet OwnershipCompatibilityUseChecker::visitCallee(
    CanSILFunctionType SubstCalleeType) {
  ParameterConvention Conv = SubstCalleeType->getCalleeConvention();
  switch (Conv) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Constant:
    assert(!SILModuleConventions(Mod).isSILIndirect(
        SILParameterInfo(SubstCalleeType, Conv)));
    return {compatibleWithOwnership(ValueOwnershipKind::Owned),
            UseLifetimeConstraint::MustBeInvalidated};
  case ParameterConvention::Indirect_In_Guaranteed:
    assert(!SILModuleConventions(Mod).isSILIndirect(
        SILParameterInfo(SubstCalleeType, Conv)));
    return {compatibleWithOwnership(ValueOwnershipKind::Owned),
            UseLifetimeConstraint::MustBeLive};
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    llvm_unreachable("Illegal convention for callee");
  case ParameterConvention::Direct_Unowned:
    if (isAddressOrTrivialType())
      return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
              UseLifetimeConstraint::MustBeLive};
    // We accept unowned, owned, and guaranteed in unowned positions.
    return {true, UseLifetimeConstraint::MustBeLive};
  case ParameterConvention::Direct_Owned:
    return {compatibleWithOwnership(ValueOwnershipKind::Owned),
            UseLifetimeConstraint::MustBeInvalidated};
  case ParameterConvention::Direct_Guaranteed:
    return {compatibleWithOwnership(ValueOwnershipKind::Guaranteed),
            UseLifetimeConstraint::MustBeLive};
  }

  llvm_unreachable("Unhandled ParameterConvention in switch.");
}

// Visit an enum value that is passed at argument position, including block
// arguments, apply arguments, and return values.
//
// The operand definition's ownership kind may be known to be "trivial",
// but it is still valid to pass that enum to a argument nontrivial type.
// For example:
//
// %val = enum $Optional<SomeClass>, #Optional.none // trivial ownership
// apply %f(%val) : (@owned Optional<SomeClass>)    // owned argument
OperandValueOwnershipKindSet OwnershipCompatibilityUseChecker::visitEnumArgument(
    EnumDecl *E, ValueOwnershipKind RequiredKind) {
  // If this value is already categorized as a trivial ownership kind, it is
  // safe to pass to any argument convention.
  if (compatibleWithOwnership(ValueOwnershipKind::Trivial)) {
    return {true, UseLifetimeConstraint::MustBeLive};
  }

  // The operand has a non-trivial ownership kind. It must match the argument
  // convention.
  auto ownership = getOwnershipKind();
  auto lifetimeConstraint = (ownership == ValueOwnershipKind::Owned)
                                ? UseLifetimeConstraint::MustBeInvalidated
                                : UseLifetimeConstraint::MustBeLive;
  return {compatibleOwnershipKinds(ownership, RequiredKind),
          lifetimeConstraint};
}

// We allow for trivial cases of enums with non-trivial cases to be passed in
// non-trivial argument positions. This fits with modeling of a
// SILFunctionArgument as a phi in a global program graph.
OperandValueOwnershipKindSet OwnershipCompatibilityUseChecker::visitApplyParameter(
    ValueOwnershipKind Kind, UseLifetimeConstraint Requirement) {
  // Check if we have an enum. If not, then we just check against the passed in
  // convention.
  EnumDecl *E = getType().getEnumOrBoundGenericEnum();
  if (!E) {
    return {compatibleWithOwnership(Kind), Requirement};
  }
  return visitEnumArgument(E, Kind);
}

// Handle Apply and TryApply.
OperandValueOwnershipKindSet OwnershipCompatibilityUseChecker::
visitFullApply(FullApplySite apply) {
  // If we are visiting the callee, handle it specially.
  if (getOperandIndex() == 0)
    return visitCallee(apply.getSubstCalleeType());

  // Indirect return arguments are address types.
  if (isAddressOrTrivialType())
    return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
            UseLifetimeConstraint::MustBeLive};

  unsigned argIndex = apply.getCalleeArgIndex(Op);
  SILParameterInfo paramInfo =
    apply.getSubstCalleeConv().getParamInfoForSILArg(argIndex);

  switch (paramInfo.getConvention()) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Direct_Owned:
    return visitApplyParameter(ValueOwnershipKind::Owned,
                               UseLifetimeConstraint::MustBeInvalidated);
  case ParameterConvention::Indirect_In_Constant:
  case ParameterConvention::Direct_Unowned:
    // We accept unowned, owned, and guaranteed in unowned positions.
    return {true, UseLifetimeConstraint::MustBeLive};
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Direct_Guaranteed:
    return visitApplyParameter(ValueOwnershipKind::Guaranteed,
                               UseLifetimeConstraint::MustBeLive);
  // The following conventions should take address types.
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    llvm_unreachable("Unexpected non-trivial parameter convention.");
  }
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitBeginApplyInst(BeginApplyInst *I) {
  return visitFullApply(I);
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitApplyInst(ApplyInst *I) {
  return visitFullApply(I);
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitTryApplyInst(TryApplyInst *I) {
  return visitFullApply(I);
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitPartialApplyInst(PartialApplyInst *I) {
  // All non-trivial types should be captured.
  if (isAddressOrTrivialType()) {
    return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
            UseLifetimeConstraint::MustBeLive};
  }
  return {compatibleWithOwnership(ValueOwnershipKind::Owned),
          UseLifetimeConstraint::MustBeInvalidated};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitYieldInst(YieldInst *I) {
  // Indirect return arguments are address types.
  if (isAddressOrTrivialType())
    return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
            UseLifetimeConstraint::MustBeLive};

  auto fnType = I->getFunction()->getLoweredFunctionType();
  auto yieldInfo = fnType->getYields()[getOperandIndex()];
  switch (yieldInfo.getConvention()) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Direct_Owned:
    return visitApplyParameter(ValueOwnershipKind::Owned,
                               UseLifetimeConstraint::MustBeInvalidated);
  case ParameterConvention::Indirect_In_Constant:
  case ParameterConvention::Direct_Unowned:
    // We accept unowned, owned, and guaranteed in unowned positions.
    return {true, UseLifetimeConstraint::MustBeLive};
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Direct_Guaranteed:
    return visitApplyParameter(ValueOwnershipKind::Guaranteed,
                               UseLifetimeConstraint::MustBeLive);
  // The following conventions should take address types.
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    llvm_unreachable("Unexpected non-trivial parameter convention.");
  }
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitAssignInst(AssignInst *I) {
  if (getValue() == I->getSrc()) {
    if (isAddressOrTrivialType()) {
      return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
              UseLifetimeConstraint::MustBeLive};
    }
    return {compatibleWithOwnership(ValueOwnershipKind::Owned),
            UseLifetimeConstraint::MustBeInvalidated};
  }

  return {true, UseLifetimeConstraint::MustBeLive};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitStoreInst(StoreInst *I) {
  if (getValue() == I->getSrc()) {
    if (isAddressOrTrivialType()) {
      return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
              UseLifetimeConstraint::MustBeLive};
    }
    return {compatibleWithOwnership(ValueOwnershipKind::Owned),
            UseLifetimeConstraint::MustBeInvalidated};
  }
  return {true, UseLifetimeConstraint::MustBeLive};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitMarkDependenceInst(
    MarkDependenceInst *MDI) {
  // We always treat mark dependence as a use that keeps a value alive. We will
  // be introducing a begin_dependence/end_dependence version of this later.
  return {true, UseLifetimeConstraint::MustBeLive};
}

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitKeyPathInst(KeyPathInst *I) {
  // KeyPath moves the value in memory out of address operands, but the
  // ownership checker doesn't reason about that yet.
  if (isAddressOrTrivialType()) {
    return {compatibleWithOwnership(ValueOwnershipKind::Trivial),
            UseLifetimeConstraint::MustBeLive};
  }
  return {compatibleWithOwnership(ValueOwnershipKind::Owned),
          UseLifetimeConstraint::MustBeInvalidated};
}

//===----------------------------------------------------------------------===//
//                            Builtin Use Checker
//===----------------------------------------------------------------------===//

namespace {

class OperandValueOwnershipKindSetClassifier
    : public SILBuiltinVisitor<OperandValueOwnershipKindSetClassifier,
                               OperandValueOwnershipKindSet> {

  const OwnershipCompatibilityUseChecker &ParentChecker;

public:
  OperandValueOwnershipKindSetClassifier(
      OwnershipCompatibilityUseChecker &ParentChecker)
      : ParentChecker(ParentChecker) {}

  SILValue getValue() const { return ParentChecker.getValue(); }

  ValueOwnershipKind getOwnershipKind() const {
    return ParentChecker.getOwnershipKind();
  }

  unsigned getOperandIndex() const { return ParentChecker.getOperandIndex(); }

  SILType getType() const { return ParentChecker.getType(); }

  bool compatibleWithOwnership(ValueOwnershipKind Kind) const {
    return ParentChecker.compatibleWithOwnership(Kind);
  }

  bool isAddressOrTrivialType() const {
    return ParentChecker.isAddressOrTrivialType();
  }

  OperandValueOwnershipKindSet visitLLVMIntrinsic(BuiltinInst *BI,
                                               llvm::Intrinsic::ID ID) {
    // LLVM intrinsics do not traffic in ownership, so if we have a result, it
    // must be trivial.
    return {true, UseLifetimeConstraint::MustBeLive};
  }

    // BUILTIN_TYPE_CHECKER_OPERATION does not live past the type checker.
#define BUILTIN_TYPE_CHECKER_OPERATION(ID, NAME)

#define BUILTIN(ID, NAME, ATTRS)                                               \
  OperandValueOwnershipKindSet visit##ID(BuiltinInst *BI, StringRef Attr);
#include "swift/AST/Builtins.def"

  OperandValueOwnershipKindSet check(BuiltinInst *BI) { return visit(BI); }
};

} // end anonymous namespace

// This is correct today since we do not have any builtins which return
// @guaranteed parameters. This means that we can only have a lifetime ending
// use with our builtins if it is owned.
#define CONSTANT_OWNERSHIP_BUILTIN(OWNERSHIP, USE_LIFETIME_CONSTRAINT, ID)     \
  OperandValueOwnershipKindSet                                                    \
      OperandValueOwnershipKindSetClassifier::visit##ID(BuiltinInst *BI,      \
                                                         StringRef Attr) {     \
    return {compatibleWithOwnership(ValueOwnershipKind::OWNERSHIP),            \
            UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT};                   \
  }
CONSTANT_OWNERSHIP_BUILTIN(Owned, MustBeLive, ErrorInMain)
CONSTANT_OWNERSHIP_BUILTIN(Owned, MustBeLive, UnexpectedError)
CONSTANT_OWNERSHIP_BUILTIN(Owned, MustBeLive, WillThrow)
CONSTANT_OWNERSHIP_BUILTIN(Owned, MustBeInvalidated, UnsafeGuaranteed)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AShr)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Add)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Alignof)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AllocRaw)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, And)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AssertConf)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AssignCopyArrayNoAlias)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AssignCopyArrayFrontToBack)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AssignCopyArrayBackToFront)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AssignTakeArray)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AssumeNonNegative)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AtomicLoad)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AtomicRMW)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, AtomicStore)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, BitCast)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, CanBeObjCClass)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, CmpXChg)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, CondUnreachable)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, CopyArray)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, DeallocRaw)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, DestroyArray)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ExactSDiv)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ExactUDiv)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ExtractElement)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FAdd)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_OEQ)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_OGE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_OGT)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_OLE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_OLT)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_ONE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_ORD)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_UEQ)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_UGE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_UGT)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_ULE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_ULT)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_UNE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FCMP_UNO)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FDiv)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FMul)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FNeg)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FPExt)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FPToSI)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FPToUI)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FPTrunc)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FRem)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, FSub)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Fence)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, GetObjCTypeEncoding)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_EQ)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_NE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_SGE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_SGT)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_SLE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_SLT)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_UGE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_UGT)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_ULE)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ICMP_ULT)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, InsertElement)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, IntToFPWithOverflow)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, IntToPtr)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, IsOptionalType)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, IsPOD)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, IsSameMetatype)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, LShr)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Mul)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, OnFastPath)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Once)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, OnceWithContext)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Or)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, PtrToInt)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SAddOver)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SDiv)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SExt)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SExtOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SIToFP)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SMulOver)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SRem)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SSubOver)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SToSCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SToUCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, SUCheckedConversion)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Shl)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Sizeof)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, StaticReport)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Strideof)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Sub)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, TakeArrayNoAlias)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, TakeArrayBackToFront)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, TakeArrayFrontToBack)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Trunc)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, TruncOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, TSanInoutAccess)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, UAddOver)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, UDiv)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, UIToFP)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, UMulOver)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, URem)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, USCheckedConversion)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, USubOver)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, UToSCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, UToUCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Unreachable)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, UnsafeGuaranteedEnd)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Xor)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ZExt)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ZExtOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, ZeroInitializer)
CONSTANT_OWNERSHIP_BUILTIN(Trivial, MustBeLive, Swift3ImplicitObjCEntrypoint)
#undef CONSTANT_OWNERSHIP_BUILTIN

// Builtins that should be lowered to SIL instructions so we should never see
// them.
#define BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(ID)                 \
  OperandValueOwnershipKindSet                                                    \
      OperandValueOwnershipKindSetClassifier::visit##ID(BuiltinInst *BI,      \
                                                         StringRef Attr) {     \
    llvm_unreachable("Builtin should have been lowered to SIL instruction?!"); \
  }
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Retain)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Release)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Autorelease)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(TryPin)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Unpin)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Load)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(LoadRaw)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(LoadInvariant)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Take)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Destroy)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Assign)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Init)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(CastToNativeObject)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(UnsafeCastToNativeObject)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(CastFromNativeObject)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(CastToBridgeObject)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(
    CastReferenceFromBridgeObject)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(
    CastBitPatternFromBridgeObject)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(BridgeToRawPointer)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(BridgeFromRawPointer)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(CastReference)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(ReinterpretCast)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(AddressOf)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(GepRaw)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(Gep)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(GetTailAddr)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(CondFail)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(FixLifetime)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(IsUnique)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(IsUniqueOrPinned)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(IsUnique_native)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(IsUniqueOrPinned_native)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(BindMemory)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(AllocWithTailElems)
BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS(ProjectTailElems)
#undef BUILTINS_THAT_SHOULD_HAVE_BEEN_LOWERED_TO_SILINSTS

OperandValueOwnershipKindSet
OwnershipCompatibilityUseChecker::visitBuiltinInst(BuiltinInst *BI) {
  return OperandValueOwnershipKindSetClassifier(*this).check(BI);
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

OperandValueOwnershipKindSet Operand::getValueOwnershipKindSet() const {

}
