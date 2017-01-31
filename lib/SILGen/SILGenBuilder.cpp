//===--- SILGenBuilder.cpp ------------------------------------------------===//
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

#include "SILGenBuilder.h"
#include "SILGenFunction.h"

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                              Utility Methods
//===----------------------------------------------------------------------===//

SILGenModule &SILGenBuilder::getSILGenModule() const { return gen.SGM; }

//===----------------------------------------------------------------------===//
//                                Constructors
//===----------------------------------------------------------------------===//

SILGenBuilder::SILGenBuilder(SILGenFunction &gen)
    : SILBuilder(gen.F), gen(gen) {}

SILGenBuilder::SILGenBuilder(SILGenFunction &gen, SILBasicBlock *insertBB)
    : SILBuilder(insertBB), gen(gen) {}

SILGenBuilder::SILGenBuilder(SILGenFunction &gen, SILBasicBlock *insertBB,
                             SmallVectorImpl<SILInstruction *> *insertedInsts)
    : SILBuilder(insertBB, insertedInsts), gen(gen) {}

SILGenBuilder::SILGenBuilder(SILGenFunction &gen, SILBasicBlock *insertBB,
                             SILBasicBlock::iterator insertInst)
    : SILBuilder(insertBB, insertInst), gen(gen) {}

//===----------------------------------------------------------------------===//
//                            Instruction Emission
//===----------------------------------------------------------------------===//

MetatypeInst *SILGenBuilder::createMetatype(SILLocation loc, SILType metatype) {
  auto theMetatype = metatype.castTo<MetatypeType>();
  // Getting a nontrivial metatype requires forcing any conformances necessary
  // to instantiate the type.
  switch (theMetatype->getRepresentation()) {
  case MetatypeRepresentation::Thin:
    break;
  case MetatypeRepresentation::Thick:
  case MetatypeRepresentation::ObjC: {
    // Walk the type recursively to look for substitutions we may need.
    theMetatype.getInstanceType().findIf([&](Type t) -> bool {
      if (!t->getAnyNominal())
        return false;

      auto subs =
          t->gatherAllSubstitutions(getSILGenModule().SwiftModule, nullptr);
      getSILGenModule().useConformancesFromSubstitutions(subs);
      return false;
    });

    break;
  }
  }

  return SILBuilder::createMetatype(loc, metatype);
}

ApplyInst *SILGenBuilder::createApply(SILLocation Loc, SILValue Fn,
                                      SILType SubstFnTy, SILType Result,
                                      ArrayRef<Substitution> Subs,
                                      ArrayRef<SILValue> Args) {
  getSILGenModule().useConformancesFromSubstitutions(Subs);
  return SILBuilder::createApply(Loc, Fn, SubstFnTy, Result, Subs, Args, false);
}

TryApplyInst *SILGenBuilder::createTryApply(SILLocation loc, SILValue Fn,
                                            SILType substFnTy,
                                            ArrayRef<Substitution> subs,
                                            ArrayRef<SILValue> args,
                                            SILBasicBlock *normalBB,
                                            SILBasicBlock *errorBB) {
  getSILGenModule().useConformancesFromSubstitutions(subs);
  return SILBuilder::createTryApply(loc, Fn, substFnTy, subs, args, normalBB,
                                    errorBB);
}

PartialApplyInst *SILGenBuilder::createPartialApply(
    SILLocation Loc, SILValue Fn, SILType SubstFnTy,
    ArrayRef<Substitution> Subs, ArrayRef<SILValue> Args, SILType ClosureTy) {
  getSILGenModule().useConformancesFromSubstitutions(Subs);
  return SILBuilder::createPartialApply(Loc, Fn, SubstFnTy, Subs, Args,
                                        ClosureTy);
}

BuiltinInst *SILGenBuilder::createBuiltin(SILLocation Loc, Identifier Name,
                                          SILType ResultTy,
                                          ArrayRef<Substitution> Subs,
                                          ArrayRef<SILValue> Args) {
  getSILGenModule().useConformancesFromSubstitutions(Subs);
  return SILBuilder::createBuiltin(Loc, Name, ResultTy, Subs, Args);
}

InitExistentialAddrInst *SILGenBuilder::createInitExistentialAddr(
    SILLocation Loc, SILValue Existential, CanType FormalConcreteType,
    SILType LoweredConcreteType,
    ArrayRef<ProtocolConformanceRef> Conformances) {
  for (auto conformance : Conformances)
    getSILGenModule().useConformance(conformance);

  return SILBuilder::createInitExistentialAddr(
      Loc, Existential, FormalConcreteType, LoweredConcreteType, Conformances);
}

InitExistentialMetatypeInst *SILGenBuilder::createInitExistentialMetatype(
    SILLocation loc, SILValue metatype, SILType existentialType,
    ArrayRef<ProtocolConformanceRef> conformances) {
  for (auto conformance : conformances)
    getSILGenModule().useConformance(conformance);

  return SILBuilder::createInitExistentialMetatype(
      loc, metatype, existentialType, conformances);
}

InitExistentialRefInst *SILGenBuilder::createInitExistentialRef(
    SILLocation Loc, SILType ExistentialType, CanType FormalConcreteType,
    SILValue Concrete, ArrayRef<ProtocolConformanceRef> Conformances) {
  for (auto conformance : Conformances)
    getSILGenModule().useConformance(conformance);

  return SILBuilder::createInitExistentialRef(
      Loc, ExistentialType, FormalConcreteType, Concrete, Conformances);
}

AllocExistentialBoxInst *SILGenBuilder::createAllocExistentialBox(
    SILLocation Loc, SILType ExistentialType, CanType ConcreteType,
    ArrayRef<ProtocolConformanceRef> Conformances) {
  for (auto conformance : Conformances)
    getSILGenModule().useConformance(conformance);

  return SILBuilder::createAllocExistentialBox(Loc, ExistentialType,
                                               ConcreteType, Conformances);
}

ManagedValue SILGenBuilder::createStructExtract(SILLocation Loc,
                                                ManagedValue Base,
                                                VarDecl *Decl) {
  ManagedValue BorrowedBase = gen.emitManagedBeginBorrow(Loc, Base.getValue());
  SILValue StructExtract =
      SILBuilder::createStructExtract(Loc, BorrowedBase.getValue(), Decl);
  return ManagedValue::forUnmanaged(StructExtract);
}

ManagedValue SILGenBuilder::createCopyValue(SILLocation Loc,
                                            ManagedValue OriginalValue) {
  SILValue Result = SILBuilder::createCopyValue(Loc, OriginalValue.getValue());
  return gen.emitManagedRValueWithCleanup(Result);
}

ManagedValue SILGenBuilder::createCopyUnownedValue(SILLocation Loc,
                                                   ManagedValue OriginalValue) {
  auto UnownedType = OriginalValue.getType().castTo<UnownedStorageType>();
  assert(UnownedType->isLoadable(ResilienceExpansion::Maximal));
  (void)UnownedType;

  SILValue Result =
      SILBuilder::createCopyUnownedValue(Loc, OriginalValue.getValue());
  return gen.emitManagedRValueWithCleanup(Result);
}

ManagedValue
SILGenBuilder::createUnsafeCopyUnownedValue(SILLocation Loc,
                                            ManagedValue OriginalValue) {
  auto UnmanagedType = OriginalValue.getType().getAs<UnmanagedStorageType>();
  SILValue Result = SILBuilder::createUnmanagedToRef(
      Loc, OriginalValue.getValue(),
      SILType::getPrimitiveObjectType(UnmanagedType.getReferentType()));
  SILBuilder::createUnmanagedRetainValue(Loc, Result);
  return gen.emitManagedRValueWithCleanup(Result);
}

ManagedValue SILGenBuilder::createOwnedPHIArgument(SILType Type) {
  SILPHIArgument *Arg =
      getInsertionBB()->createPHIArgument(Type, ValueOwnershipKind::Owned);
  return gen.emitManagedRValueWithCleanup(Arg);
}

ManagedValue SILGenBuilder::createTupleExtract(SILLocation Loc, ManagedValue Base, unsigned Index,
                                               SILType Type) {
  ManagedValue BorrowedBase = gen.emitManagedBeginBorrow(Loc, Base.getValue());
  SILValue TupleExtract =
    SILBuilder::createTupleExtract(Loc, BorrowedBase.getValue(), Index, Type);
  return ManagedValue::forUnmanaged(TupleExtract);
}

ManagedValue SILGenBuilder::createTupleExtract(SILLocation Loc, ManagedValue Value, unsigned Index) {
  SILType Type = Value.getType().getTupleElementType(Index);
  return createTupleExtract(Loc, Value, Index, Type);
}

ManagedValue SILGenBuilder::createAllocRef(SILLocation Loc, SILType RefType, bool objc, bool canAllocOnStack,
                                           ArrayRef<SILType> InputElementTypes,
                                           ArrayRef<ManagedValue> InputElementCountOperands) {
  llvm::SmallVector<SILType, 8> ElementTypes(InputElementTypes.begin(),
                                             InputElementTypes.end());
  llvm::SmallVector<SILValue, 8> ElementCountOperands;
  std::transform(std::begin(InputElementCountOperands),
                 std::end(InputElementCountOperands),
                 std::back_inserter(ElementCountOperands),
                 [](ManagedValue M) -> SILValue { return M.getValue(); });

  AllocRefInst *ARI =
    SILBuilder::createAllocRef(Loc, RefType, objc, canAllocOnStack,
                               ElementTypes, ElementCountOperands);
  return gen.emitManagedRValueWithCleanup(ARI);
}

ManagedValue SILGenBuilder::createAllocRefDynamic(SILLocation Loc, ManagedValue Operand, SILType RefType, bool objc,
                                                  ArrayRef<SILType> InputElementTypes,
                                                  ArrayRef<ManagedValue> InputElementCountOperands) {
  llvm::SmallVector<SILType, 8> ElementTypes(InputElementTypes.begin(),
                                             InputElementTypes.end());
  llvm::SmallVector<SILValue, 8> ElementCountOperands;
  std::transform(std::begin(InputElementCountOperands),
                 std::end(InputElementCountOperands),
                 std::back_inserter(ElementCountOperands),
                 [](ManagedValue M) -> SILValue { return M.getValue(); });

  AllocRefDynamicInst *ARDI =
    SILBuilder::createAllocRefDynamic(Loc, Operand.getValue(), RefType, objc,
                                      ElementTypes, ElementCountOperands);
  return gen.emitManagedRValueWithCleanup(ARDI);
}

ManagedValue SILGenBuilder::createUncheckedEnumData(SILLocation loc, ManagedValue operand, EnumElementDecl *element) {
  if (operand.hasCleanup()) {
    SILValue newValue = SILBuilder::createUncheckedEnumData(loc, operand.forward(gen), element);
    return gen.emitManagedRValueWithCleanup(newValue);
  }

  ManagedValue borrowedBase = operand.borrow(gen, loc);
  SILValue newValue = SILBuilder::createUncheckedEnumData(loc, borrowedBase.getValue(), element);
  return ManagedValue::forUnmanaged(newValue);
}

ManagedValue SILGenBuilder::
createUncheckedTakeEnumDataAddr(SILLocation Loc, ManagedValue Operand,
                                EnumElementDecl *Element, SILType Ty) {
  // First see if we have a cleanup. If we do, we are going to forward and emit
  // a managed buffer with cleanup.
  if (Operand.hasCleanup()) {
    return gen.emitManagedBufferWithCleanup(
        SILBuilder::createUncheckedTakeEnumDataAddr(Loc, Operand.forward(gen),
                                                    Element, Ty));
  }

  SILValue V = SILBuilder::createUncheckedTakeEnumDataAddr(
      Loc, Operand.getUnmanagedValue(), Element, Ty);
  if (Operand.isLValue())
    return ManagedValue::forLValue(V);
  return ManagedValue::forUnmanaged(V);
}

ManagedValue SILGenBuilder::createLoadTake(SILLocation loc, ManagedValue v) {
  auto &lowering = getFunction().getTypeLowering(v.getType());
  return createLoadTake(loc, v, lowering);
}

ManagedValue SILGenBuilder::createLoadTake(SILLocation loc, ManagedValue v,
                                           const TypeLowering &lowering) {
  assert(lowering.getLoweredType().getAddressType() == v.getType());
  SILValue result =
      lowering.emitLoadOfCopy(*this, loc, v.forward(gen), IsTake);
  if (lowering.isTrivial())
    return ManagedValue::forUnmanaged(result);
  assert(!lowering.isAddressOnly() && "cannot retain an unloadable type");
  return gen.emitManagedRValueWithCleanup(result, lowering);
}

ManagedValue SILGenBuilder::createLoadCopy(SILLocation loc, ManagedValue v) {
  auto &lowering = getFunction().getTypeLowering(v.getType());
  return createLoadCopy(loc, v, lowering);
}

ManagedValue SILGenBuilder::createLoadCopy(SILLocation loc, ManagedValue v,
                                           const TypeLowering &lowering) {
  assert(lowering.getLoweredType().getAddressType() == v.getType());
  SILValue result =
      lowering.emitLoadOfCopy(*this, loc, v.forward(gen), IsNotTake);
  if (lowering.isTrivial())
    return ManagedValue::forUnmanaged(result);
  assert(!lowering.isAddressOnly() && "cannot retain an unloadable type");
  return gen.emitManagedRValueWithCleanup(result, lowering);
}

ManagedValue SILGenBuilder::createFunctionArgument(SILType Type, ValueDecl *Decl) {
  SILFunction &F = getFunction();

  SILFunctionArgument *Arg = F.begin()->createFunctionArgument(Type, Decl);
  if (Arg->getType().isObject()) {
    if (Arg->getOwnershipKind().isTrivialOr(ValueOwnershipKind::Owned))
      return gen.emitManagedRValueWithCleanup(Arg);
    return ManagedValue::forBorrowedRValue(Arg);
  }

  return gen.emitManagedBufferWithCleanup(Arg);
}

ManagedValue SILGenBuilder::createMarkUninitialized(
    ValueDecl *Decl, ManagedValue Operand, MarkUninitializedInst::Kind MUKind) {
  bool isOwned =
    Operand.getOwnershipKind() == ValueOwnershipKind::Owned;

  // We either have an owned or trivial value.
  SILValue Value =
    SILBuilder::createMarkUninitialized(Decl, Operand.forward(gen), MUKind);
  assert(Value->getType().isObject() && "Expected only objects here");

  if (!isOwned) {
    return ManagedValue::forUnmanaged(Value);
  }

  return gen.emitManagedRValueWithCleanup(Value);
}

ManagedValue SILGenBuilder::createEnum(SILLocation Loc, ManagedValue Payload, EnumElementDecl *Decl,
                                       SILType Type) {
  SILValue Enum = SILBuilder::createEnum(Loc, Payload.forward(gen), Decl, Type);
  if (Enum.getOwnershipKind() != ValueOwnershipKind::Owned)
    return ManagedValue::forUnmanaged(Enum);
  return gen.emitManagedRValueWithCleanup(Enum);
}

ManagedValue SILGenBuilder::createUpcast(SILLocation Loc, ManagedValue Original, SILType Type) {
  bool hadCleanup = Original.hasCleanup();
  bool isLValue = Original.isLValue();

  SILValue convertedValue = SILBuilder::createUpcast(Loc, Original.forward(gen), Type);

  if (isLValue) {
    return ManagedValue::forLValue(convertedValue);
  }

  if (!hadCleanup) {
    return ManagedValue::forUnmanaged(convertedValue);
  }

  if (Type.isAddress()) {
    return gen.emitManagedBufferWithCleanup(convertedValue);
  }

  return gen.emitManagedRValueWithCleanup(convertedValue);
}

ManagedValue SILGenBuilder::createLoadBorrow(SILLocation Loc, ManagedValue Original) {
  assert(Original.getType().isAddress());
  LoadBorrowInst *LBI = SILBuilder::createLoadBorrow(Loc, Original.getValue());
  return gen.emitManagedBorrowedRValueWithCleanup(Original.getValue(),
                                                  LBI);
}

ManagedValue SILGenBuilder::createTupleElementAddr(SILLocation Loc, ManagedValue Base, unsigned Index,
                                                   SILType Type) {
  ManagedValue BorrowedBase = gen.emitManagedBeginBorrow(Loc, Base.getValue());
  SILValue TupleEltAddr =
    SILBuilder::createTupleElementAddr(Loc, BorrowedBase.getValue(), Index, Type);
  return ManagedValue::forUnmanaged(TupleEltAddr);
}

ManagedValue SILGenBuilder::createTupleElementAddr(SILLocation Loc, ManagedValue Value, unsigned Index) {
  SILType Type = Value.getType().getTupleElementType(Index);
  return createTupleExtract(Loc, Value, Index, Type);
}
