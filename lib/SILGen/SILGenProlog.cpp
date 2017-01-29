//===--- SILGenProlog.cpp - Function prologue emission --------------------===//
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

#include "SILGenFunction.h"
#include "Initialization.h"
#include "ManagedValue.h"
#include "Scope.h"
#include "swift/SIL/SILArgument.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ParameterList.h"
#include "swift/Basic/Fallthrough.h"

using namespace swift;
using namespace Lowering;

SILValue SILGenFunction::emitSelfDecl(VarDecl *selfDecl) {
  // Emit the implicit 'self' argument.
  SILType selfType = getLoweredLoadableType(selfDecl->getType());
  SILValue selfValue = F.begin()->createFunctionArgument(selfType, selfDecl);
  VarLocs[selfDecl] = VarLoc::get(selfValue);
  SILLocation PrologueLoc(selfDecl);
  PrologueLoc.markAsPrologue();
  unsigned ArgNo = 1; // Hardcoded for destructors.
  B.createDebugValue(PrologueLoc, selfValue, {selfDecl->isLet(), ArgNo});
  return selfValue;
}

namespace {

/// Cleanup that writes back to an inout argument on function exit.
class CleanupWriteBackToInOut : public Cleanup {
  VarDecl *var;
  SILValue inoutAddr;

public:
  CleanupWriteBackToInOut(VarDecl *var, SILValue inoutAddr)
    : var(var), inoutAddr(inoutAddr) {}

  void emit(SILGenFunction &gen, CleanupLocation l) override {
    // Assign from the local variable to the inout address with an
    // 'autogenerated' copyaddr.
    l.markAutoGenerated();
    gen.B.createCopyAddr(l, gen.VarLocs[var].value, inoutAddr,
                         IsNotTake, IsNotInitialization);
  }
};
} // end anonymous namespace

  
namespace {
class StrongReleaseCleanup : public Cleanup {
  SILValue box;
public:
  StrongReleaseCleanup(SILValue box) : box(box) {}
  void emit(SILGenFunction &gen, CleanupLocation l) override {
    gen.B.emitDestroyValueOperation(l, box);
  }
    void dump() const override {
#ifndef NDEBUG
      llvm::errs() << "DeallocateValueBuffer\n"
                   << "State: " << getState()
                   << "box: " << box << "\n";
#endif
    }
};
} // end anonymous namespace


namespace {
class EmitBBArguments : public CanTypeVisitor<EmitBBArguments,
                                              /*RetTy*/ ManagedValue>
{
public:
  SILGenFunction &gen;
  SILBasicBlock *parent;
  SILLocation loc;
  bool functionArgs;
  ArrayRef<SILParameterInfo> &parameters;

  EmitBBArguments(SILGenFunction &gen, SILBasicBlock *parent,
                  SILLocation l, bool functionArgs,
                  ArrayRef<SILParameterInfo> &parameters)
    : gen(gen), parent(parent), loc(l), functionArgs(functionArgs),
      parameters(parameters) {}

  ManagedValue getManagedValue(SILValue arg, CanType t,
                               SILParameterInfo parameterInfo) const {
    switch (parameterInfo.getConvention()) {
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Indirect_In_Guaranteed:
      // If we have a guaranteed parameter, it is passed in at +0, and its
      // lifetime is guaranteed. We can potentially use the argument as-is
      // if the parameter is bound as a 'let' without cleaning up.
      return ManagedValue::forUnmanaged(arg);

    case ParameterConvention::Direct_Unowned:
      // An unowned parameter is passed at +0, like guaranteed, but it isn't
      // kept alive by the caller, so we need to retain and manage it
      // regardless.
      return gen.emitManagedRetain(loc, arg);

    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable:
      // An inout parameter is +0 and guaranteed, but represents an lvalue.
      return ManagedValue::forLValue(arg);

    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Indirect_In:
      // An owned or 'in' parameter is passed in at +1. We can claim ownership
      // of the parameter and clean it up when it goes out of scope.
      return gen.emitManagedRValueWithCleanup(arg);
    }
    llvm_unreachable("bad parameter convention");
  }

  ManagedValue visitType(CanType t) {
    auto argType = gen.getLoweredType(t);
    // Pop the next parameter info.
    auto parameterInfo = parameters.front();
    parameters = parameters.slice(1);
    assert(
        argType
            == parent->getParent()->mapTypeIntoContext(
                   gen.getSILType(parameterInfo))
        && "argument does not have same type as specified by parameter info");

    SILValue arg =
        parent->createFunctionArgument(argType, loc.getAsASTNode<ValueDecl>());
    ManagedValue mv = getManagedValue(arg, t, parameterInfo);

    // If the value is a (possibly optional) ObjC block passed into the entry
    // point of the function, then copy it so we can treat the value reliably
    // as a heap object. Escape analysis can eliminate this copy if it's
    // unneeded during optimization.
    CanType objectType = t;
    if (auto theObjTy = t.getAnyOptionalObjectType())
      objectType = theObjTy;
    if (functionArgs
        && isa<FunctionType>(objectType)
        && cast<FunctionType>(objectType)->getRepresentation()
              == FunctionType::Representation::Block) {
      SILValue blockCopy = gen.B.createCopyBlock(loc, mv.getValue());
      mv = gen.emitManagedRValueWithCleanup(blockCopy);
    }
    return mv;
  }

  ManagedValue visitTupleType(CanTupleType t) {
    SmallVector<ManagedValue, 4> elements;

    auto &tl = gen.getTypeLowering(t);
    bool canBeGuaranteed = tl.isLoadable();

    // Collect the exploded elements.
    for (auto fieldType : t.getElementTypes()) {
      auto elt = visit(fieldType);
      // If we can't borrow one of the elements as a guaranteed parameter, then
      // we have to +1 the tuple.
      if (elt.hasCleanup())
        canBeGuaranteed = false;
      elements.push_back(elt);
    }

    if (tl.isLoadable()) {
      SmallVector<SILValue, 4> elementValues;
      if (canBeGuaranteed) {
        // If all of the elements were guaranteed, we can form a guaranteed tuple.
        for (auto element : elements)
          elementValues.push_back(element.getUnmanagedValue());
      } else {
        // Otherwise, we need to move or copy values into a +1 tuple.
        for (auto element : elements) {
          SILValue value = element.hasCleanup()
            ? element.forward(gen)
            : element.copyUnmanaged(gen, loc).forward(gen);
          elementValues.push_back(value);
        }
      }
      auto tupleValue = gen.B.createTuple(loc, tl.getLoweredType(),
                                          elementValues);
      return canBeGuaranteed
        ? ManagedValue::forUnmanaged(tupleValue)
        : gen.emitManagedRValueWithCleanup(tupleValue);
    } else {
      // If the type is address-only, we need to move or copy the elements into
      // a tuple in memory.
      // TODO: It would be a bit more efficient to use a preallocated buffer
      // in this case.
      auto buffer = gen.emitTemporaryAllocation(loc, tl.getLoweredType());
      for (auto i : indices(elements)) {
        auto element = elements[i];
        auto elementBuffer = gen.B.createTupleElementAddr(loc, buffer,
                                        i, element.getType().getAddressType());
        if (element.hasCleanup())
          element.forwardInto(gen, loc, elementBuffer);
        else
          element.copyInto(gen, elementBuffer, loc);
      }
      return gen.emitManagedRValueWithCleanup(buffer);
    }
  }
};
} // end anonymous namespace

  
namespace {

/// A helper for creating SILArguments and binding variables to the argument
/// names.
struct ArgumentInitHelper {
  SILGenFunction &gen;
  SILFunction &f;
  SILGenBuilder &initB;

  /// An ArrayRef that we use in our SILParameterList queue. Parameters are
  /// sliced off of the front as they're emitted.
  ArrayRef<SILParameterInfo> parameters;
  unsigned ArgNo = 0;

  ArgumentInitHelper(SILGenFunction &gen, SILFunction &f)
    : gen(gen), f(f), initB(gen.B),
      parameters(f.getLoweredFunctionType()->getParameters()) {
  }

  unsigned getNumArgs() const { return ArgNo; }

  ManagedValue makeArgument(Type ty, SILBasicBlock *parent, SILLocation l) {
    assert(ty && "no type?!");

    // Create an RValue by emitting destructured arguments into a basic block.
    CanType canTy = ty->getCanonicalType();
    return EmitBBArguments(gen, parent, l, /*functionArgs*/ true,
                           parameters).visit(canTy);
  }

  /// Create a SILArgument and store its value into the given Initialization,
  /// if not null.
  void makeArgumentIntoBinding(Type ty, SILBasicBlock *parent, VarDecl *vd) {
    SILLocation loc(vd);
    loc.markAsPrologue();

    ManagedValue argrv = makeArgument(ty, parent, loc);

    // Create a shadow copy of inout parameters so they can be captured
    // by closures. The InOutDeshadowing guaranteed optimization will
    // eliminate the variable if it is not needed.
    if (auto inOutTy = vd->getType()->getAs<InOutType>()) {

      SILValue address = argrv.getUnmanagedValue();

      CanType objectType = inOutTy->getObjectType()->getCanonicalType();

      // As a special case, don't introduce a local variable for
      // Builtin.UnsafeValueBuffer, which is not copyable.
      if (isa<BuiltinUnsafeValueBufferType>(objectType)) {
        // FIXME: mark a debug location?
        gen.VarLocs[vd] = SILGenFunction::VarLoc::get(address);
        gen.B.createDebugValueAddr(loc, address, {vd->isLet(), ArgNo});
        return;
      }
      assert(argrv.getType().isAddress() && "expected inout to be address");
    } else {
      assert(vd->isLet() && "expected parameter to be immutable!");
      // If the variable is immutable, we can bind the value as is.
      // Leave the cleanup on the argument, if any, in place to consume the
      // argument if we're responsible for it.
    }
    gen.VarLocs[vd] = SILGenFunction::VarLoc::get(argrv.getValue());
    if (argrv.getType().isAddress())
      gen.B.createDebugValueAddr(loc, argrv.getValue(), {vd->isLet(), ArgNo});
    else
      gen.B.createDebugValue(loc, argrv.getValue(), {vd->isLet(), ArgNo});
  }

  void emitParam(ParamDecl *PD) {
    // The contextual type of a ParamDecl has DynamicSelfType. We don't want
    // that here.
    auto type = PD->getType()->eraseDynamicSelfType();

    ++ArgNo;
    if (PD->hasName()) {
      makeArgumentIntoBinding(type, &*f.begin(), PD);
      return;
    }

    emitAnonymousParam(type, PD, PD);
  }

  void emitAnonymousParam(Type type, SILLocation paramLoc, ParamDecl *PD) {
    // Allow non-materializable tuples to be bound to anonymous parameters.
    if (!type->isMaterializable()) {
      if (auto tupleType = type->getAs<TupleType>()) {
        for (auto eltType : tupleType->getElementTypes()) {
          emitAnonymousParam(eltType, paramLoc, nullptr);
        }
        return;
      }
    }

    // A value bound to _ is unused and can be immediately released.
    Scope discardScope(gen.Cleanups, CleanupLocation(PD));

    // Manage the parameter.
    ManagedValue argrv = makeArgument(type, &*f.begin(), paramLoc);

    // Don't do anything else if we don't have a parameter.
    if (!PD) return;

    // Emit debug information for the argument.
    SILLocation loc(PD);
    loc.markAsPrologue();
    if (argrv.getType().isAddress())
      gen.B.createDebugValueAddr(loc, argrv.getValue(), {PD->isLet(), ArgNo});
    else
      gen.B.createDebugValue(loc, argrv.getValue(), {PD->isLet(), ArgNo});
  }
};
} // end anonymous namespace

  
static void makeArgument(Type ty, ParamDecl *decl,
                         SmallVectorImpl<SILValue> &args, SILGenFunction &gen) {
  assert(ty && "no type?!");
  
  // Destructure tuple arguments.
  if (TupleType *tupleTy = ty->getAs<TupleType>()) {
    for (auto fieldType : tupleTy->getElementTypes())
      makeArgument(fieldType, decl, args, gen);
  } else {
    auto arg =
        gen.F.begin()->createFunctionArgument(gen.getLoweredType(ty), decl);
    args.push_back(arg);
  }
}


void SILGenFunction::bindParametersForForwarding(const ParameterList *params,
                                     SmallVectorImpl<SILValue> &parameters) {
  for (auto param : *params) {
    Type type = (param->hasType()
                 ? param->getType()->eraseDynamicSelfType()
                 : F.mapTypeIntoContext(param->getInterfaceType()));
    makeArgument(type, param, parameters, *this);
  }
}

static void emitCaptureArguments(SILGenFunction &gen,
                                 AnyFunctionRef closure,
                                 CapturedValue capture,
                                 unsigned ArgNo) {

  auto *VD = capture.getDecl();
  SILLocation Loc(VD);
  Loc.markAsPrologue();

  // Local function to get the captured variable type within the capturing
  // context.
  auto getVarTypeInCaptureContext = [&]() -> Type {
    auto interfaceType = cast<VarDecl>(VD)->getInterfaceType();
    if (!interfaceType->hasTypeParameter()) return interfaceType;

    // NB: The generic signature may be elided from the lowered function type
    // if the function is in a fully-specialized context, but we still need to
    // canonicalize references to the generic parameters that may appear in
    // non-canonical types in that context. We need the original generic
    // environment from the AST for that.
    auto genericEnv = closure.getGenericEnvironment();
    return genericEnv->mapTypeIntoContext(gen.F.getModule().getSwiftModule(),
                                          interfaceType);
  };

  switch (gen.SGM.Types.getDeclCaptureKind(capture)) {
  case CaptureKind::None:
    break;

  case CaptureKind::Constant: {
    auto type = getVarTypeInCaptureContext();
    auto &lowering = gen.getTypeLowering(type);
    // Constant decls are captured by value.
    SILType ty = lowering.getLoweredType();
    SILValue val = gen.F.begin()->createFunctionArgument(ty, VD);

    // If the original variable was settable, then Sema will have treated the
    // VarDecl as an lvalue, even in the closure's use.  As such, we need to
    // allow formation of the address for this captured value.  Create a
    // temporary within the closure to provide this address.
    if (VD->isSettable(VD->getDeclContext())) {
      auto addr = gen.emitTemporaryAllocation(VD, ty);
      lowering.emitStore(gen.B, VD, val, addr, StoreOwnershipQualifier::Init);
      val = addr;
    }

    gen.VarLocs[VD] = SILGenFunction::VarLoc::get(val);
    if (auto *AllocStack = dyn_cast<AllocStackInst>(val))
      AllocStack->setArgNo(ArgNo);
    else 
      gen.B.createDebugValue(Loc, val, {/*Constant*/true, ArgNo});

    // TODO: Closure contexts should always be guaranteed.
    if (!gen.SGM.M.getOptions().EnableGuaranteedClosureContexts
        && !lowering.isTrivial())
      gen.enterDestroyCleanup(val);
    break;
  }

  case CaptureKind::Box: {
    // LValues are captured as a retained @box that owns
    // the captured value.
    auto type = getVarTypeInCaptureContext();
    auto boxTy = gen.SGM.Types.getContextBoxTypeForCapture(VD,
                               gen.getLoweredType(type).getSwiftRValueType(),
                               gen.F.getGenericEnvironment(), /*mutable*/ true);
    SILValue box = gen.F.begin()->createFunctionArgument(
        SILType::getPrimitiveObjectType(boxTy), VD);
    SILValue addr = gen.B.createProjectBox(VD, box, 0);
    gen.VarLocs[VD] = SILGenFunction::VarLoc::get(addr, box);
    gen.B.createDebugValueAddr(Loc, addr, {/*Constant*/false, ArgNo});
    if (!gen.SGM.M.getOptions().EnableGuaranteedClosureContexts)
      gen.Cleanups.pushCleanup<StrongReleaseCleanup>(box);
    break;
  }
  case CaptureKind::StorageAddress: {
    // Non-escaping stored decls are captured as the address of the value.
    auto type = getVarTypeInCaptureContext();
    SILType ty = gen.getLoweredType(type).getAddressType();
    SILValue addr = gen.F.begin()->createFunctionArgument(ty, VD);
    gen.VarLocs[VD] = SILGenFunction::VarLoc::get(addr);
    gen.B.createDebugValueAddr(Loc, addr, {/*Constant*/true, ArgNo});
    break;
  }
  }
}

void SILGenFunction::emitProlog(AnyFunctionRef TheClosure,
                                ArrayRef<ParameterList*> paramPatterns,
                                Type resultType, bool throws) {
  unsigned ArgNo = emitProlog(paramPatterns, resultType,
                              TheClosure.getAsDeclContext(), throws);

  // Emit the capture argument variables. These are placed last because they
  // become the first curry level of the SIL function.
  auto captureInfo = SGM.Types.getLoweredLocalCaptures(TheClosure);
  for (auto capture : captureInfo.getCaptures()) {
    if (capture.isDynamicSelfMetadata()) {
      auto selfMetatype = MetatypeType::get(
          captureInfo.getDynamicSelfType()->getSelfType(),
          MetatypeRepresentation::Thick)
              ->getCanonicalType();
      SILType ty = SILType::getPrimitiveObjectType(selfMetatype);
      SILValue val = F.begin()->createFunctionArgument(ty);
      (void) val;

      return;
    }

    emitCaptureArguments(*this, TheClosure, capture, ++ArgNo);
  }
}

static void emitIndirectResultParameters(SILGenFunction &gen, Type resultType,
                                         DeclContext *DC) {
  // Expand tuples.
  if (auto tupleType = resultType->getAs<TupleType>()) {
    for (auto eltType : tupleType->getElementTypes()) {
      emitIndirectResultParameters(gen, eltType, DC);
    }
    return;
  }

  // If the return type is address-only, emit the indirect return argument.

  const TypeLowering &resultTI =
      gen.getTypeLowering(DC->mapTypeIntoContext(resultType));
  if (!SILModuleConventions::isReturnedIndirectlyInSIL(
          resultTI.getLoweredType(), gen.SGM.M)) {
    return;
  }
  auto &ctx = gen.getASTContext();
  auto var = new (ctx) ParamDecl(/*IsLet*/ false, SourceLoc(), SourceLoc(),
                                 ctx.getIdentifier("$return_value"), SourceLoc(),
                                 ctx.getIdentifier("$return_value"), Type(),
                                 DC);
  var->setInterfaceType(resultType);

  auto *arg =
      gen.F.begin()->createFunctionArgument(resultTI.getLoweredType(), var);
  (void)arg;
}

unsigned SILGenFunction::emitProlog(ArrayRef<ParameterList *> paramLists,
                                    Type resultType, DeclContext *DC,
                                    bool throws) {
  // Create the indirect result parameters.
  if (auto *genericSig = DC->getGenericSignatureOfContext()) {
    resultType = genericSig->getCanonicalTypeInContext(
      resultType, *SGM.M.getSwiftModule());
  }

  emitIndirectResultParameters(*this, resultType, DC);

  // Emit the argument variables in calling convention order.
  ArgumentInitHelper emitter(*this, F);

  for (ParameterList *paramList : reversed(paramLists)) {
    // Add the SILArguments and use them to initialize the local argument
    // values.
    for (auto &param : *paramList)
      emitter.emitParam(param);
  }

  // Record the ArgNo of the artificial $error inout argument. 
  unsigned ArgNo = emitter.getNumArgs();
  if (throws) {
    RegularLocation Loc{SourceLoc()};
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC))
      Loc = AFD->getThrowsLoc();
    else if (auto *ACE = dyn_cast<AbstractClosureExpr>(DC))
      Loc = ACE->getLoc();
    auto NativeErrorTy = SILType::getExceptionType(getASTContext());
    ManagedValue Undef = emitUndef(Loc, NativeErrorTy);
    B.createDebugValue(Loc, Undef.getValue(),
                       {"$error", /*Constant*/ false, ++ArgNo});
  }

  return ArgNo;
}

