//===--- SILInstruction.cpp - Instructions for SIL code -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the high-level SILInstruction classes used for SIL code.
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILInstruction.h"
#include "swift/Basic/type_traits.h"
#include "swift/Basic/Unicode.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/AST/AST.h"
#include "swift/Basic/AssertImplements.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"
using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
// Instruction-specific properties on SILValue
//===----------------------------------------------------------------------===//

SILLocation SILInstruction::getLoc() const { return Location.getLocation(); }

const SILDebugScope *SILInstruction::getDebugScope() const {
  return Location.getScope();
}

void SILInstruction::setDebugScope(SILBuilder &B, const SILDebugScope *DS) {
  if (getDebugScope() && getDebugScope()->InlinedCallSite)
    assert(DS->InlinedCallSite && "throwing away inlined scope info");

  assert(DS->getParentFunction() == getFunction() &&
         "scope belongs to different function");

  Location = SILDebugLocation(getLoc(), DS);
}

//===----------------------------------------------------------------------===//
// ilist_traits<SILInstruction> Implementation
//===----------------------------------------------------------------------===//

// The trait object is embedded into a basic block.  Use dirty hacks to
// reconstruct the BB from the 'self' pointer of the trait.
SILBasicBlock *llvm::ilist_traits<SILInstruction>::getContainingBlock() {
  size_t Offset(
      size_t(&((SILBasicBlock *)0->*SILBasicBlock::getSublistAccess())));
  iplist<SILInstruction> *Anchor(static_cast<iplist<SILInstruction> *>(this));
  return reinterpret_cast<SILBasicBlock *>(reinterpret_cast<char *>(Anchor) -
                                           Offset);
}


void llvm::ilist_traits<SILInstruction>::addNodeToList(SILInstruction *I) {
  assert(I->ParentBB == 0 && "Already in a list!");
  I->ParentBB = getContainingBlock();
}

void llvm::ilist_traits<SILInstruction>::removeNodeFromList(SILInstruction *I) {
  // When an instruction is removed from a BB, clear the parent pointer.
  assert(I->ParentBB && "Not in a list!");
  I->ParentBB = 0;
}

void llvm::ilist_traits<SILInstruction>::
transferNodesFromList(llvm::ilist_traits<SILInstruction> &L2,
                      llvm::ilist_iterator<SILInstruction> first,
                      llvm::ilist_iterator<SILInstruction> last) {
  // If transferring instructions within the same basic block, no reason to
  // update their parent pointers.
  SILBasicBlock *ThisParent = getContainingBlock();
  if (ThisParent == L2.getContainingBlock()) return;

  // Update the parent fields in the instructions.
  for (; first != last; ++first)
    first->ParentBB = ThisParent;
}

//===----------------------------------------------------------------------===//
// SILInstruction Implementation
//===----------------------------------------------------------------------===//

// Assert that all subclasses of ValueBase implement classof.
#define VALUE(CLASS, PARENT) \
  ASSERT_IMPLEMENTS_STATIC(CLASS, PARENT, classof, bool(const ValueBase*));
#include "swift/SIL/SILNodes.def"

SILFunction *SILInstruction::getFunction() {
  return getParent()->getParent();
}
const SILFunction *SILInstruction::getFunction() const {
  return getParent()->getParent();
}

SILModule &SILInstruction::getModule() const {
  return getFunction()->getModule();
}

/// removeFromParent - This method unlinks 'self' from the containing basic
/// block, but does not delete it.
///
void SILInstruction::removeFromParent() {
  getParent()->remove(this);
}

/// eraseFromParent - This method unlinks 'self' from the containing basic
/// block and deletes it.
///
void SILInstruction::eraseFromParent() {
  assert(use_empty() && "There are uses of instruction being deleted.");
  getParent()->erase(this);
}

/// Unlink this instruction from its current basic block and insert it into
/// the basic block that Later lives in, right before Later.
void SILInstruction::moveBefore(SILInstruction *Later) {
  if (this == Later)
    return;

  getParent()->remove(this);
  Later->getParent()->insert(Later, this);
}

/// Unlink this instruction from its current basic block and insert it into
/// the basic block that Earlier lives in, right after Earlier.
void SILInstruction::moveAfter(SILInstruction *Earlier) {
  // Since MovePos is an instruction, we know that there is always a valid
  // iterator after it.
  auto Later = std::next(SILBasicBlock::iterator(Earlier));
  moveBefore(&*Later);
}

void SILInstruction::dropAllReferences() {
  MutableArrayRef<Operand> PossiblyDeadOps = getAllOperands();
  for (auto OpI = PossiblyDeadOps.begin(),
            OpE = PossiblyDeadOps.end(); OpI != OpE; ++OpI) {
    OpI->drop();
  }

  // If we have a function ref inst, we need to especially drop its function
  // argument so that it gets a proper ref decrement.
  auto *FRI = dyn_cast<FunctionRefInst>(this);
  if (!FRI || !FRI->getReferencedFunction())
    return;

  FRI->dropReferencedFunction();
}

void SILInstruction::replaceAllUsesWithUndef() {
  SILModule &Mod = getModule();
  while (!use_empty()) {
    Operand *Op = *use_begin();
    Op->set(SILUndef::get(Op->get()->getType(), Mod));
  }
}

namespace {
  class InstructionDestroyer : public SILVisitor<InstructionDestroyer> {
  public:
#define VALUE(CLASS, PARENT) void visit##CLASS(CLASS *I) { I->~CLASS(); }
#include "swift/SIL/SILNodes.def"
  };
} // end anonymous namespace

void SILInstruction::destroy(SILInstruction *I) {
  InstructionDestroyer().visit(I);
}

namespace {
  /// Given a pair of instructions that are already known to have the same kind,
  /// type, and operands check any special state in the two instructions that
  /// could disrupt equality.
  class InstructionIdentityComparer :
    public SILInstructionVisitor<InstructionIdentityComparer, bool> {
  public:

    InstructionIdentityComparer(const SILInstruction *L) : LHS(L) { }

    /// Make sure we only process instructions we know how to process.
    bool visitValueBase(const ValueBase *RHS) {
      return false;
    }

    bool visitInjectEnumAddrInst(const InjectEnumAddrInst *RHS) {
        auto *X = cast<InjectEnumAddrInst>(LHS);
        return X->getElement() == RHS->getElement();
    }

    bool visitDestroyAddrInst(const DestroyAddrInst *RHS) {
      return true;
    }

    bool visitReleaseValueInst(const ReleaseValueInst *RHS) {
      return true;
    }

    bool visitRetainValueInst(const RetainValueInst *RHS) {
      return true;
    }

    bool visitDeallocStackInst(const DeallocStackInst *RHS) {
      return true;
    }

    bool visitAllocStackInst(const AllocStackInst *RHS) {
      return true;
    }

    bool visitDeallocBoxInst(const DeallocBoxInst *RHS) {
      return true;
    }

    bool visitAllocBoxInst(const AllocBoxInst *RHS) {
      return true;
    }

    bool visitDeallocRefInst(const DeallocRefInst *RHS) {
      return true;
    }

    bool visitDeallocPartialRefInst(const DeallocPartialRefInst *RHS) {
      return true;
    }

    bool visitAllocRefInst(const AllocRefInst *RHS) {
      auto *LHSInst = cast<AllocRefInst>(LHS);
      auto LHSTypes = LHSInst->getTailAllocatedTypes();
      auto RHSTypes = RHS->getTailAllocatedTypes();
      unsigned NumTypes = LHSTypes.size();
      assert(NumTypes == RHSTypes.size());
      for (unsigned Idx = 0; Idx < NumTypes; ++Idx) {
        if (LHSTypes[Idx] != RHSTypes[Idx])
          return false;
      }
      return true;
    }

    bool visitAllocRefDynamicInst(const AllocRefDynamicInst *RHS) {
      return true;
    }

    bool visitProjectValueBufferInst(const ProjectValueBufferInst *RHS) {
      auto *X = cast<ProjectValueBufferInst>(LHS);
      return X->getValueType() == RHS->getValueType();
    }

    bool visitProjectBoxInst(const ProjectBoxInst *RHS) {
      return true;
    }

    bool visitProjectExistentialBoxInst(const ProjectExistentialBoxInst *RHS) {
      return true;
    }

    bool visitStrongReleaseInst(const StrongReleaseInst *RHS) {
      return true;
    }

    bool visitStrongRetainInst(const StrongRetainInst *RHS) {
      return true;
    }

    bool visitStrongRetainUnownedInst(const StrongRetainUnownedInst *RHS) {
      return true;
    }

    bool visitLoadInst(const LoadInst *RHS) {
      auto LHSQualifier = cast<LoadInst>(LHS)->getOwnershipQualifier();
      return LHSQualifier == RHS->getOwnershipQualifier();
    }

    bool visitLoadBorrowInst(const LoadBorrowInst *RHS) { return true; }

    bool visitEndBorrowInst(const EndBorrowInst *RHS) { return true; }
    bool visitBeginBorrowInst(const BeginBorrowInst *BBI) { return true; }

    bool visitStoreBorrowInst(const StoreBorrowInst *RHS) {
      auto *X = cast<StoreBorrowInst>(LHS);
      return X->getSrc() == RHS->getSrc() && X->getDest() == RHS->getDest();
    }

    bool visitStoreInst(const StoreInst *RHS) {
      auto *X = cast<StoreInst>(LHS);
      return X->getSrc() == RHS->getSrc() && X->getDest() == RHS->getDest() &&
        X->getOwnershipQualifier() == RHS->getOwnershipQualifier();
    }

    bool visitBindMemoryInst(const BindMemoryInst *RHS) {
      auto *X = cast<BindMemoryInst>(LHS);
      return X->getBoundType() == RHS->getBoundType();
    }

    bool visitFunctionRefInst(const FunctionRefInst *RHS) {
      auto *X = cast<FunctionRefInst>(LHS);
      return X->getReferencedFunction() == RHS->getReferencedFunction();
    }

    bool visitAllocGlobalInst(const AllocGlobalInst *RHS) {
      auto *X = cast<AllocGlobalInst>(LHS);
      return X->getReferencedGlobal() == RHS->getReferencedGlobal();
    }

    bool visitGlobalAddrInst(const GlobalAddrInst *RHS) {
      auto *X = cast<GlobalAddrInst>(LHS);
      return X->getReferencedGlobal() == RHS->getReferencedGlobal();
    }

    bool visitIntegerLiteralInst(const IntegerLiteralInst *RHS) {
      APInt X = cast<IntegerLiteralInst>(LHS)->getValue();
      APInt Y = RHS->getValue();
      return X.getBitWidth() == Y.getBitWidth() &&
        X == Y;
    }

    bool visitFloatLiteralInst(const FloatLiteralInst *RHS) {
      // Avoid floating point comparison issues by doing a bitwise comparison.
      APInt X = cast<FloatLiteralInst>(LHS)->getBits();
      APInt Y = RHS->getBits();
      return X.getBitWidth() == Y.getBitWidth() &&
        X == Y;
    }

    bool visitStringLiteralInst(const StringLiteralInst *RHS) {
      auto LHS_ = cast<StringLiteralInst>(LHS);
      return LHS_->getEncoding() == RHS->getEncoding()
        && LHS_->getValue().equals(RHS->getValue());
    }

    bool visitStructInst(const StructInst *RHS) {
      // We have already checked the operands. Make sure that the StructDecls
      // match up.
      StructDecl *S1 = cast<StructInst>(LHS)->getStructDecl();
      return S1 == RHS->getStructDecl();
    }

    bool visitStructExtractInst(const StructExtractInst *RHS) {
      // We have already checked that the operands of our struct_extracts
      // match. Thus we need to check the field/struct decl which are not
      // operands.
      auto *X = cast<StructExtractInst>(LHS);
      if (X->getStructDecl() != RHS->getStructDecl())
        return false;
      if (X->getField() != RHS->getField())
        return false;
      return true;
    }

    bool visitRefElementAddrInst(RefElementAddrInst *RHS) {
      auto *X = cast<RefElementAddrInst>(LHS);
      if (X->getField() != RHS->getField())
        return false;
      if (X->getOperand() != RHS->getOperand())
        return false;
      return true;
    }

    bool visitRefTailAddrInst(RefTailAddrInst *RHS) {
      auto *X = cast<RefTailAddrInst>(LHS);
      if (X->getTailType() != RHS->getTailType())
        return false;
      return true;
    }

    bool visitStructElementAddrInst(const StructElementAddrInst *RHS) {
      // We have already checked that the operands of our struct_element_addrs
      // match. Thus we only need to check the field/struct decl which are not
      // operands.
      auto *X = cast<StructElementAddrInst>(LHS);
      if (X->getStructDecl() != RHS->getStructDecl())
        return false;
      if (X->getField() != RHS->getField())
        return false;
      return true;
    }

    bool visitTupleInst(const TupleInst *RHS) {
      // We have already checked the operands. Make sure that the tuple types
      // match up.
      TupleType *TT1 = cast<TupleInst>(LHS)->getTupleType();
      return TT1 == RHS->getTupleType();
    }

    bool visitTupleExtractInst(const TupleExtractInst *RHS) {
      // We have already checked that the operands match. Thus we only need to
      // check the field no and tuple type which are not represented as operands.
      auto *X = cast<TupleExtractInst>(LHS);
      if (X->getTupleType() != RHS->getTupleType())
        return false;
      if (X->getFieldNo() != RHS->getFieldNo())
        return false;
      return true;
    }

    bool visitTupleElementAddrInst(const TupleElementAddrInst *RHS) {
      // We have already checked that the operands match. Thus we only need to
      // check the field no and tuple type which are not represented as operands.
      auto *X = cast<TupleElementAddrInst>(LHS);
      if (X->getTupleType() != RHS->getTupleType())
        return false;
      if (X->getFieldNo() != RHS->getFieldNo())
        return false;
      return true;
    }

    bool visitMetatypeInst(const MetatypeInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitValueMetatypeInst(const ValueMetatypeInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitExistentialMetatypeInst(const ExistentialMetatypeInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitIndexRawPointerInst(IndexRawPointerInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitIndexAddrInst(IndexAddrInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitTailAddrInst(TailAddrInst *RHS) {
      auto *X = cast<TailAddrInst>(LHS);
      if (X->getTailType() != RHS->getTailType())
        return false;
      return true;
    }

    bool visitCondFailInst(CondFailInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitApplyInst(ApplyInst *RHS) {
      auto *X = cast<ApplyInst>(LHS);
      return X->getSubstitutions() == RHS->getSubstitutions();
    }

    bool visitBuiltinInst(BuiltinInst *RHS) {
      auto *X = cast<BuiltinInst>(LHS);
      if (X->getName() != RHS->getName())
        return false;
      return X->getSubstitutions() == RHS->getSubstitutions();
    }

    bool visitEnumInst(EnumInst *RHS) {
      // We already checked operands and types. Only thing we need to check is
      // that the element is the same.
      auto *X = cast<EnumInst>(LHS);
      return X->getElement() == RHS->getElement();
    }

    bool visitUncheckedEnumDataInst(UncheckedEnumDataInst *RHS) {
      // We already checked operands and types. Only thing we need to check is
      // that the element is the same.
      auto *X = cast<UncheckedEnumDataInst>(LHS);
      return X->getElement() == RHS->getElement();
    }

    bool visitSelectEnumInstBase(const SelectEnumInstBase *RHS) {
      // Check that the instructions match cases in the same order.
      auto *X = cast<SelectEnumInstBase>(LHS);

      if (X->getNumCases() != RHS->getNumCases())
        return false;
      if (X->hasDefault() != RHS->hasDefault())
        return false;

      for (unsigned i = 0, e = X->getNumCases(); i < e; ++i) {
        if (X->getCase(i).first != RHS->getCase(i).first)
          return false;
      }

      return true;
    }

    bool visitSelectEnumInst(const SelectEnumInst *RHS) {
      return visitSelectEnumInstBase(RHS);
    }
    bool visitSelectEnumAddrInst(const SelectEnumAddrInst *RHS) {
      return visitSelectEnumInstBase(RHS);
    }

    bool visitSelectValueInst(const SelectValueInst *RHS) {
      // Check that the instructions match cases in the same order.
      auto *X = cast<SelectValueInst>(LHS);

      if (X->getNumCases() != RHS->getNumCases())
        return false;
      if (X->hasDefault() != RHS->hasDefault())
        return false;

      for (unsigned i = 0, e = X->getNumCases(); i < e; ++i) {
        if (X->getCase(i).first != RHS->getCase(i).first)
          return false;
        if (X->getCase(i).second != RHS->getCase(i).second)
          return false;
      }

      return true;
    }

    // Conversion instructions.
    // All of these just return true as they have already had their
    // operands and types checked
    bool visitUncheckedRefCastInst(UncheckedRefCastInst *RHS) {
      return true;
    }

    bool visitUncheckedAddrCastInst(UncheckedAddrCastInst *RHS) {
      return true;
    }

    bool visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *RHS) {
      return true;
    }

    bool visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *RHS) {
      return true;
    }

    bool visitUpcastInst(UpcastInst *RHS) {
      return true;
    }

    bool visitAddressToPointerInst(AddressToPointerInst *RHS) {
      return true;
    }

    bool visitPointerToAddressInst(PointerToAddressInst *RHS) {
      return cast<PointerToAddressInst>(LHS)->isStrict() == RHS->isStrict();
    }

    bool visitRefToRawPointerInst(RefToRawPointerInst *RHS) {
      return true;
    }

    bool visitRawPointerToRefInst(RawPointerToRefInst *RHS) {
      return true;
    }

    bool visitRefToUnownedInst(RefToUnownedInst *RHS) {
      return true;
    }

    bool visitUnownedToRefInst(UnownedToRefInst *RHS) {
      return true;
    }

    bool visitRefToUnmanagedInst(RefToUnmanagedInst *RHS) {
      return true;
    }

    bool visitUnmanagedToRefInst(UnmanagedToRefInst *RHS) {
      return true;
    }

    bool visitThinToThickFunctionInst(ThinToThickFunctionInst *RHS) {
      return true;
    }

    bool visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *RHS) {
      return true;
    }

    bool visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *RHS) {
      return true;
    }

    bool visitConvertFunctionInst(ConvertFunctionInst *RHS) {
      return true;
    }

    bool visitObjCMetatypeToObjectInst(ObjCMetatypeToObjectInst *RHS) {
      return true;
    }

    bool visitObjCExistentialMetatypeToObjectInst(ObjCExistentialMetatypeToObjectInst *RHS) {
      return true;
    }

    bool visitProjectBlockStorageInst(ProjectBlockStorageInst *RHS) {
      return true;
    }

    bool visitIsNonnullInst(IsNonnullInst *RHS) {
      return true;
    }
      
    bool visitBridgeObjectToRefInst(BridgeObjectToRefInst *X) {
      return true;
    }

    bool visitBridgeObjectToWordInst(BridgeObjectToWordInst *X) {
      return true;
    }
      
    bool visitRefToBridgeObjectInst(RefToBridgeObjectInst *X) {
      return true;
    }
    bool visitThinFunctionToPointerInst(ThinFunctionToPointerInst *X) {
      return true;
    }
    bool visitPointerToThinFunctionInst(PointerToThinFunctionInst *X) {
      return true;
    }

    bool visitObjCProtocolInst(ObjCProtocolInst *RHS) {
      auto *X = cast<ObjCProtocolInst>(LHS);
      return X->getProtocol() == RHS->getProtocol();
    }

    bool visitClassMethodInst(ClassMethodInst *RHS) {
      auto *X = cast<ClassMethodInst>(LHS);
      return X->getMember()  == RHS->getMember() &&
             X->getOperand() == RHS->getOperand() &&
             X->getType()    == RHS->getType();
    }

    bool visitWitnessMethodInst(const WitnessMethodInst *RHS) {
      auto *X = cast<WitnessMethodInst>(LHS);
      if (X->isVolatile() != RHS->isVolatile())
        return false;
      if (X->getMember() != RHS->getMember())
        return false;
      if (X->getLookupType() != RHS->getLookupType())
        return false;
      if (X->getConformance() != RHS->getConformance())
        return false;
      return true;
    }

    bool visitMarkDependenceInst(const MarkDependenceInst *RHS) {
       return true;
    }

    bool visitOpenExistentialRefInst(const OpenExistentialRefInst *RHS) {
      return true;
    }

  private:
    const SILInstruction *LHS;
  };
}

bool SILInstruction::hasIdenticalState(const SILInstruction *RHS) const {
  SILInstruction *UnconstRHS = const_cast<SILInstruction *>(RHS);
  return InstructionIdentityComparer(this).visit(UnconstRHS);
}

namespace {
  class AllOperandsAccessor : public SILVisitor<AllOperandsAccessor,
                                                ArrayRef<Operand> > {
  public:
#define VALUE(CLASS, PARENT) \
    ArrayRef<Operand> visit##CLASS(const CLASS *I) {                    \
      llvm_unreachable("accessing non-instruction " #CLASS);            \
    }
#define INST(CLASS, PARENT, TEXTUALNAME, MEMBEHAVIOR, RELEASINGBEHAVIOR)       \
  ArrayRef<Operand> visit##CLASS(const CLASS *I) {                             \
    ASSERT_IMPLEMENTS(CLASS, SILInstruction, getAllOperands,                   \
                      ArrayRef<Operand>() const);                              \
    return I->getAllOperands();                                                \
  }
#include "swift/SIL/SILNodes.def"
  };

  class AllOperandsMutableAccessor
    : public SILVisitor<AllOperandsMutableAccessor,
                        MutableArrayRef<Operand> > {
  public:
#define VALUE(CLASS, PARENT) \
    MutableArrayRef<Operand> visit##CLASS(const CLASS *I) {             \
      llvm_unreachable("accessing non-instruction " #CLASS);            \
    }
#define INST(CLASS, PARENT, TEXTUALNAME, MEMBEHAVIOR, RELEASINGBEHAVIOR)       \
  MutableArrayRef<Operand> visit##CLASS(CLASS *I) {                            \
    ASSERT_IMPLEMENTS(CLASS, SILInstruction, getAllOperands,                   \
                      MutableArrayRef<Operand>());                             \
    return I->getAllOperands();                                                \
  }
#include "swift/SIL/SILNodes.def"
  };

#define IMPLEMENTS_METHOD(DerivedClass, BaseClass, MemberName, ExpectedType)  \
  (!::std::is_same<BaseClass, GET_IMPLEMENTING_CLASS(DerivedClass, MemberName,\
                                                     ExpectedType)>::value)

  class TypeDependentOperandsAccessor
      : public SILVisitor<TypeDependentOperandsAccessor,
                          ArrayRef<Operand>> {
  public:
#define VALUE(CLASS, PARENT) \
    ArrayRef<Operand> visit##CLASS(const CLASS *I) {                    \
      llvm_unreachable("accessing non-instruction " #CLASS);            \
    }
#define INST(CLASS, PARENT, TEXTUALNAME, MEMBEHAVIOR, RELEASINGBEHAVIOR)       \
  ArrayRef<Operand> visit##CLASS(const CLASS *I) {                             \
    if (!IMPLEMENTS_METHOD(CLASS, SILInstruction, getTypeDependentOperands,    \
                           ArrayRef<Operand>() const))                         \
      return {};                                                               \
    return I->getTypeDependentOperands();                                      \
  }
#include "swift/SIL/SILNodes.def"
  };

  class TypeDependentOperandsMutableAccessor
    : public SILVisitor<TypeDependentOperandsMutableAccessor,
                        MutableArrayRef<Operand> > {
  public:
#define VALUE(CLASS, PARENT) \
    MutableArrayRef<Operand> visit##CLASS(const CLASS *I) {             \
      llvm_unreachable("accessing non-instruction " #CLASS);            \
    }
#define INST(CLASS, PARENT, TEXTUALNAME, MEMBEHAVIOR, RELEASINGBEHAVIOR)       \
  MutableArrayRef<Operand> visit##CLASS(CLASS *I) {                            \
    if (!IMPLEMENTS_METHOD(CLASS, SILInstruction, getTypeDependentOperands,    \
                           MutableArrayRef<Operand>()))                        \
      return {};                                                               \
    return I->getTypeDependentOperands();                                      \
  }
#include "swift/SIL/SILNodes.def"
  };
} // end anonymous namespace

ArrayRef<Operand> SILInstruction::getAllOperands() const {
  return AllOperandsAccessor().visit(const_cast<SILInstruction *>(this));
}

MutableArrayRef<Operand> SILInstruction::getAllOperands() {
  return AllOperandsMutableAccessor().visit(this);
}

ArrayRef<Operand> SILInstruction::getTypeDependentOperands() const {
  return TypeDependentOperandsAccessor().visit(
      const_cast<SILInstruction *>(this));
}

MutableArrayRef<Operand> SILInstruction::getTypeDependentOperands() {
  return TypeDependentOperandsMutableAccessor().visit(this);
}

/// getOperandNumber - Return which operand this is in the operand list of the
/// using instruction.
unsigned Operand::getOperandNumber() const {
  return this - &cast<SILInstruction>(getUser())->getAllOperands()[0];
}

SILInstruction::MemoryBehavior SILInstruction::getMemoryBehavior() const {

  if (auto *BI = dyn_cast<BuiltinInst>(this)) {
    // Handle Swift builtin functions.
    const BuiltinInfo &BInfo = BI->getBuiltinInfo();
    if (BInfo.ID != BuiltinValueKind::None)
      return BInfo.isReadNone() ? MemoryBehavior::None
                                : MemoryBehavior::MayHaveSideEffects;

    // Handle LLVM intrinsic functions.
    const IntrinsicInfo & IInfo = BI->getIntrinsicInfo();
    if (IInfo.ID != llvm::Intrinsic::not_intrinsic) {
      // Read-only.
      if (IInfo.hasAttribute(llvm::Attribute::ReadOnly) &&
          IInfo.hasAttribute(llvm::Attribute::NoUnwind))
        return MemoryBehavior::MayRead;
      // Read-none?
      return IInfo.hasAttribute(llvm::Attribute::ReadNone) &&
                     IInfo.hasAttribute(llvm::Attribute::NoUnwind)
                 ? MemoryBehavior::None
                 : MemoryBehavior::MayHaveSideEffects;
    }
  }

  // Handle full apply sites that have a resolvable callee function with an
  // effects attribute.
  if (isa<FullApplySite>(this)) {
    FullApplySite Site(const_cast<SILInstruction *>(this));
    if (auto *F = Site.getCalleeFunction()) {
      return F->getEffectsKind() == EffectsKind::ReadNone
                 ? MemoryBehavior::None
                 : MemoryBehavior::MayHaveSideEffects;
    }
  }

  switch (getKind()) {
#define INST(CLASS, PARENT, TEXTUALNAME, MEMBEHAVIOR, RELEASINGBEHAVIOR)       \
  case ValueKind::CLASS:                                                       \
    return MemoryBehavior::MEMBEHAVIOR;
#include "swift/SIL/SILNodes.def"
  case ValueKind::SILArgument:
  case ValueKind::SILUndef:
    llvm_unreachable("Non-instructions are unreachable.");
  }
  llvm_unreachable("We've just exhausted the switch.");
}

SILInstruction::ReleasingBehavior SILInstruction::getReleasingBehavior() const {
  switch (getKind()) {
#define INST(CLASS, PARENT, TEXTUALNAME, MEMBEHAVIOR, RELEASINGBEHAVIOR)       \
  case ValueKind::CLASS:                                                       \
    return ReleasingBehavior::RELEASINGBEHAVIOR;
#include "swift/SIL/SILNodes.def"
 case ValueKind::SILArgument:
 case ValueKind::SILUndef:
   llvm_unreachable("Non-instructions are unreachable.");
  }
  llvm_unreachable("We've just exhausted the switch.");
}

bool SILInstruction::mayHaveSideEffects() const {
  // If this instruction traps then it must have side effects.
  if (mayTrap())
    return true;

  MemoryBehavior B = getMemoryBehavior();
  return B == MemoryBehavior::MayWrite ||
    B == MemoryBehavior::MayReadWrite ||
    B == MemoryBehavior::MayHaveSideEffects;
}

bool SILInstruction::mayRelease() const {
  if (getReleasingBehavior() ==
      SILInstruction::ReleasingBehavior::DoesNotRelease)
    return false;

  switch (getKind()) {
  default:
    llvm_unreachable("Unhandled releasing instruction!");

  case ValueKind::ApplyInst:
  case ValueKind::TryApplyInst:
  case ValueKind::DestroyAddrInst:
  case ValueKind::StrongReleaseInst:
  case ValueKind::UnownedReleaseInst:
  case ValueKind::ReleaseValueInst:
    return true;

  case ValueKind::UnconditionalCheckedCastAddrInst: {
    // Failing casts with take_always can release.
    auto *Cast = cast<UnconditionalCheckedCastAddrInst>(this);
    return Cast->getConsumptionKind() == CastConsumptionKind::TakeAlways;
  }
  case ValueKind::CheckedCastAddrBranchInst: {
    // Failing casts with take_always can release.
    auto *Cast = cast<CheckedCastAddrBranchInst>(this);
    return Cast->getConsumptionKind() == CastConsumptionKind::TakeAlways;
  }

  case ValueKind::CopyAddrInst: {
    auto *CopyAddr = cast<CopyAddrInst>(this);
    // copy_addr without initialization can cause a release.
    return CopyAddr->isInitializationOfDest() ==
           IsInitialization_t::IsNotInitialization;
  }

  case ValueKind::BuiltinInst: {
    auto *BI = cast<BuiltinInst>(this);
    // Builtins without side effects also do not release.
    if (!BI->mayHaveSideEffects())
      return false;
    // If this is a builtin which might have side effect, but its side
    // effects do not cause reference counts to be decremented, return false.
    if (auto Kind = BI->getBuiltinKind()) {
      switch (Kind.getValue()) {
        case BuiltinValueKind::CopyArray:
          return false;
        default:
          break;
      }
    }
    if (auto ID = BI->getIntrinsicID()) {
      switch (ID.getValue()) {
        case llvm::Intrinsic::memcpy:
        case llvm::Intrinsic::memmove:
        case llvm::Intrinsic::memset:
          return false;
        default:
          break;
      }
    }
    return true;
  }
  }
}

bool SILInstruction::mayReleaseOrReadRefCount() const {
  switch (getKind()) {
  case ValueKind::IsUniqueInst:
  case ValueKind::IsUniqueOrPinnedInst:
    return true;
  default:
    return mayRelease();
  }
}

namespace {
  class TrivialCloner : public SILCloner<TrivialCloner> {
    friend class SILCloner<TrivialCloner>;
    friend class SILVisitor<TrivialCloner>;
    SILInstruction *Result = nullptr;
    TrivialCloner(SILFunction *F) : SILCloner(*F) {}
  public:

    static SILInstruction *doIt(SILInstruction *I) {
      TrivialCloner TC(I->getFunction());
      TC.visit(I);
      return TC.Result;
    }

    void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
      assert(Orig->getFunction() == &getBuilder().getFunction() &&
             "cloning between functions is not supported");

      Result = Cloned;
      SILCloner<TrivialCloner>::postProcess(Orig, Cloned);
    }
    SILValue remapValue(SILValue Value) {
      return Value;
    }
    SILBasicBlock *remapBasicBlock(SILBasicBlock *BB) { return BB; }
  };
}

bool SILInstruction::isAllocatingStack() const {
  if (isa<AllocStackInst>(this))
    return true;

  if (auto *ARI = dyn_cast<AllocRefInst>(this)) {
    if (ARI->canAllocOnStack())
      return true;
  }
  return false;
}

bool SILInstruction::isDeallocatingStack() const {
  if (isa<DeallocStackInst>(this))
    return true;

  if (auto *DRI = dyn_cast<DeallocRefInst>(this)) {
    if (DRI->canAllocOnStack())
      return true;
  }
  return false;
}


/// Create a new copy of this instruction, which retains all of the operands
/// and other information of this one.  If an insertion point is specified,
/// then the new instruction is inserted before the specified point, otherwise
/// the new instruction is returned without a parent.
SILInstruction *SILInstruction::clone(SILInstruction *InsertPt) {
  SILInstruction *NewInst = TrivialCloner::doIt(this);

  if (NewInst && InsertPt)
    InsertPt->getParent()->insert(InsertPt, NewInst);
  return NewInst;
}

/// Returns true if the instruction can be duplicated without any special
/// additional handling. It is important to know this information when
/// you perform such optimizations like e.g. jump-threading.
bool SILInstruction::isTriviallyDuplicatable() const {
  if (isa<AllocStackInst>(this) || isa<DeallocStackInst>(this)) {
    return false;
  }
  if (auto *ARI = dyn_cast<AllocRefInst>(this)) {
    if (ARI->canAllocOnStack())
      return false;
  }

  if (isa<OpenExistentialAddrInst>(this) ||
      isa<OpenExistentialRefInst>(this) ||
      isa<OpenExistentialMetatypeInst>(this)) {
    // Don't know how to duplicate these properly yet. Inst.clone() per
    // instruction does not work. Because the follow-up instructions need to
    // reuse the same archetype uuid which would only work if we used a
    // cloner.
    return false;
  }

  if (auto *MI = dyn_cast<MethodInst>(this)) {
    // We can't build SSA for method values that lower to objc methods.
    if (MI->getMember().isForeign)
      return false;
  }

  return true;
}

bool SILInstruction::mayTrap() const {
  switch(getKind()) {
  case ValueKind::CondFailInst:
  case ValueKind::UnconditionalCheckedCastInst:
  case ValueKind::UnconditionalCheckedCastAddrInst:
    return true;
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
//                           Ownership Kind Lookup
//===----------------------------------------------------------------------===//

namespace {

class ValueOwnershipKindVisitor
    : public SILVisitor<ValueOwnershipKindVisitor, ValueOwnershipKind> {

public:
  ValueOwnershipKindVisitor() = default;
  ~ValueOwnershipKindVisitor() = default;
  ValueOwnershipKindVisitor(const ValueOwnershipKindVisitor &) = delete;
  ValueOwnershipKindVisitor(ValueOwnershipKindVisitor &&) = delete;

  ValueOwnershipKind visitForwardingInst(SILInstruction *I);
  ValueOwnershipKind visitPHISILArgument(SILArgument *Arg);

  ValueOwnershipKind visitValueBase(ValueBase *V) {
    llvm_unreachable("unimplemented method on ValueBaseOwnershipVisitor");
  }
#define VALUE(Id, Parent) ValueOwnershipKind visit##Id(Id *ID);
#include "swift/SIL/SILNodes.def"
};

} // end anonymous namespace

#define NO_OWNERSHIP_INST(INST)                                                \
  ValueOwnershipKind ValueOwnershipKindVisitor::visit##INST##Inst(             \
      INST##Inst *V) {                                                         \
    assert(!V->hasValue() && "Expected instruction to not have value");        \
    return ValueOwnershipKind::None;                                           \
  }
NO_OWNERSHIP_INST(AllocGlobal)
NO_OWNERSHIP_INST(Assign)
NO_OWNERSHIP_INST(AutoreleaseValue)
NO_OWNERSHIP_INST(BindMemory)
NO_OWNERSHIP_INST(Branch)
NO_OWNERSHIP_INST(CheckedCastAddrBranch)
NO_OWNERSHIP_INST(CheckedCastBranch)
NO_OWNERSHIP_INST(CondBranch)
NO_OWNERSHIP_INST(CondFail)
NO_OWNERSHIP_INST(CopyAddr)
NO_OWNERSHIP_INST(DeallocBox)
NO_OWNERSHIP_INST(DeallocExistentialBox)
NO_OWNERSHIP_INST(DeallocPartialRef)
NO_OWNERSHIP_INST(DeallocRef)
NO_OWNERSHIP_INST(DeallocStack)
NO_OWNERSHIP_INST(DeallocValueBuffer)
NO_OWNERSHIP_INST(DebugValue)
NO_OWNERSHIP_INST(DebugValueAddr)
NO_OWNERSHIP_INST(DeinitExistentialAddr)
NO_OWNERSHIP_INST(DestroyAddr)
NO_OWNERSHIP_INST(DestroyValue)
NO_OWNERSHIP_INST(DynamicMethodBranch)
NO_OWNERSHIP_INST(EndBorrow)
NO_OWNERSHIP_INST(FixLifetime)
NO_OWNERSHIP_INST(InjectEnumAddr)
NO_OWNERSHIP_INST(MarkFunctionEscape)
NO_OWNERSHIP_INST(MarkUninitializedBehavior)
NO_OWNERSHIP_INST(ReleaseValue)
NO_OWNERSHIP_INST(RetainValue)
NO_OWNERSHIP_INST(Return)
NO_OWNERSHIP_INST(SetDeallocating)
NO_OWNERSHIP_INST(Store)
NO_OWNERSHIP_INST(StoreUnowned)
NO_OWNERSHIP_INST(StoreWeak)
NO_OWNERSHIP_INST(StrongRelease)
NO_OWNERSHIP_INST(StrongRetain)
NO_OWNERSHIP_INST(StrongRetainUnowned)
NO_OWNERSHIP_INST(StrongUnpin)
NO_OWNERSHIP_INST(SwitchEnum)
NO_OWNERSHIP_INST(SwitchEnumAddr)
NO_OWNERSHIP_INST(SwitchValue)
NO_OWNERSHIP_INST(TailAddr)
NO_OWNERSHIP_INST(Throw)
NO_OWNERSHIP_INST(TryApply)
NO_OWNERSHIP_INST(UnownedRelease)
NO_OWNERSHIP_INST(UnownedRetain)
NO_OWNERSHIP_INST(Unreachable)
#undef NO_OWNERSHIP_INST

#define CONSTANT_OWNERSHIP_INST(INST, OWNERSHIP)                               \
  ValueOwnershipKind ValueOwnershipKindVisitor::visit##INST##Inst(             \
      INST##Inst *Arg) {                                                       \
    assert(Arg->hasValue() && "Expected to have a result");                    \
    return ValueOwnershipKind::OWNERSHIP;                                      \
  }
CONSTANT_OWNERSHIP_INST(MarkUninitialized, Trivial)
CONSTANT_OWNERSHIP_INST(AddressToPointer, Trivial)
CONSTANT_OWNERSHIP_INST(AllocBox, Owned)
CONSTANT_OWNERSHIP_INST(AllocExistentialBox, Owned)
CONSTANT_OWNERSHIP_INST(AllocRef, Owned)
CONSTANT_OWNERSHIP_INST(AllocRefDynamic, Owned)
CONSTANT_OWNERSHIP_INST(AllocStack, Unowned)
CONSTANT_OWNERSHIP_INST(AllocValueBuffer, Owned)
CONSTANT_OWNERSHIP_INST(BridgeObjectToWord, Trivial)
CONSTANT_OWNERSHIP_INST(ClassMethod, Trivial)
CONSTANT_OWNERSHIP_INST(CopyBlock, Owned)
CONSTANT_OWNERSHIP_INST(CopyValue, Owned)
CONSTANT_OWNERSHIP_INST(DynamicMethod, Trivial)
CONSTANT_OWNERSHIP_INST(ExistentialMetatype, Trivial) // Is this right?
CONSTANT_OWNERSHIP_INST(FloatLiteral, Trivial)
CONSTANT_OWNERSHIP_INST(FunctionRef, Trivial)
CONSTANT_OWNERSHIP_INST(GlobalAddr, Trivial)
CONSTANT_OWNERSHIP_INST(IndexAddr, None)
CONSTANT_OWNERSHIP_INST(IndexRawPointer, None)
CONSTANT_OWNERSHIP_INST(InitBlockStorageHeader, None)
CONSTANT_OWNERSHIP_INST(InitEnumDataAddr, None)
CONSTANT_OWNERSHIP_INST(InitExistentialAddr, None)
CONSTANT_OWNERSHIP_INST(InitExistentialMetatype, Trivial)
CONSTANT_OWNERSHIP_INST(IntegerLiteral, Trivial)
CONSTANT_OWNERSHIP_INST(IsNonnull, Trivial)
CONSTANT_OWNERSHIP_INST(IsUnique, Trivial)
CONSTANT_OWNERSHIP_INST(IsUniqueOrPinned, Trivial)
CONSTANT_OWNERSHIP_INST(LoadBorrow, Guaranteed)
CONSTANT_OWNERSHIP_INST(BeginBorrow, Guaranteed)
CONSTANT_OWNERSHIP_INST(StoreBorrow, Guaranteed)
CONSTANT_OWNERSHIP_INST(LoadUnowned, Owned)
CONSTANT_OWNERSHIP_INST(LoadWeak, Owned)
CONSTANT_OWNERSHIP_INST(Metatype, Trivial)
CONSTANT_OWNERSHIP_INST(ObjCExistentialMetatypeToObject, Trivial)
CONSTANT_OWNERSHIP_INST(ObjCMetatypeToObject, Trivial)
CONSTANT_OWNERSHIP_INST(ObjCProtocol, Trivial) // Is this right?
CONSTANT_OWNERSHIP_INST(ObjCToThickMetatype, Trivial)
CONSTANT_OWNERSHIP_INST(OpenExistentialAddr, None)
CONSTANT_OWNERSHIP_INST(OpenExistentialBox, Trivial)
CONSTANT_OWNERSHIP_INST(OpenExistentialMetatype, Trivial)
CONSTANT_OWNERSHIP_INST(PartialApply, Owned)
CONSTANT_OWNERSHIP_INST(PointerToAddress, None)
CONSTANT_OWNERSHIP_INST(PointerToThinFunction, Trivial)
CONSTANT_OWNERSHIP_INST(ProjectBlockStorage, None)
CONSTANT_OWNERSHIP_INST(ProjectBox, None)
CONSTANT_OWNERSHIP_INST(ProjectExistentialBox, None)
CONSTANT_OWNERSHIP_INST(ProjectValueBuffer, None)
CONSTANT_OWNERSHIP_INST(RawPointerToRef, Unowned)
CONSTANT_OWNERSHIP_INST(RefElementAddr, None)
CONSTANT_OWNERSHIP_INST(RefTailAddr, None)
CONSTANT_OWNERSHIP_INST(RefToRawPointer, Trivial)
CONSTANT_OWNERSHIP_INST(RefToUnmanaged, Trivial)
CONSTANT_OWNERSHIP_INST(RefToUnowned, Unowned)
CONSTANT_OWNERSHIP_INST(StringLiteral, Trivial)
CONSTANT_OWNERSHIP_INST(StrongPin, Owned)
CONSTANT_OWNERSHIP_INST(StructElementAddr, None)
CONSTANT_OWNERSHIP_INST(SuperMethod, Trivial)
CONSTANT_OWNERSHIP_INST(ThickToObjCMetatype, Trivial)
CONSTANT_OWNERSHIP_INST(ThinFunctionToPointer, Trivial)
CONSTANT_OWNERSHIP_INST(ThinToThickFunction, Trivial)
CONSTANT_OWNERSHIP_INST(TupleElementAddr, None)
CONSTANT_OWNERSHIP_INST(UncheckedAddrCast, None)
CONSTANT_OWNERSHIP_INST(UncheckedBitwiseCast, Trivial)
CONSTANT_OWNERSHIP_INST(UncheckedRefCastAddr, None)
CONSTANT_OWNERSHIP_INST(UncheckedTakeEnumDataAddr, None)
CONSTANT_OWNERSHIP_INST(UncheckedTrivialBitCast, Trivial)
CONSTANT_OWNERSHIP_INST(UnconditionalCheckedCastAddr, None)
CONSTANT_OWNERSHIP_INST(UnmanagedToRef, Unowned)
CONSTANT_OWNERSHIP_INST(UnownedToRef, Unowned)
CONSTANT_OWNERSHIP_INST(ValueMetatype, Trivial)
CONSTANT_OWNERSHIP_INST(WitnessMethod, Trivial)
CONSTANT_OWNERSHIP_INST(SelectEnumAddr, Trivial)
#undef CONSTANT_OWNERSHIP_INST

// For a forwarding instruction, we loop over all pre
ValueOwnershipKind
ValueOwnershipKindVisitor::visitForwardingInst(SILInstruction *I) {
  ArrayRef<Operand> Ops = I->getAllOperands();
  if (Ops.empty())
    return ValueOwnershipKind::Trivial;
  llvm::Optional<ValueOwnershipKind> Base = Ops[0].get()->getOwnershipKind();
  for (const Operand &Op : Ops.slice(1)) {
    Base = ValueOwnershipKindMerge(Base, Op.get()->getOwnershipKind());
  }
  return Base.getValue();
}

#define FORWARDING_OWNERSHIP_INST(INST)                                        \
  ValueOwnershipKind ValueOwnershipKindVisitor::visit##INST##Inst(             \
      INST##Inst *I) {                                                         \
    assert(I->hasValue() && "Expected to have a value");                       \
    return visitForwardingInst(I);                                             \
  }
FORWARDING_OWNERSHIP_INST(BridgeObjectToRef)
FORWARDING_OWNERSHIP_INST(ConvertFunction)
FORWARDING_OWNERSHIP_INST(Enum)
FORWARDING_OWNERSHIP_INST(InitExistentialRef)
FORWARDING_OWNERSHIP_INST(OpenExistentialRef)
FORWARDING_OWNERSHIP_INST(RefToBridgeObject)
FORWARDING_OWNERSHIP_INST(SelectEnum)
FORWARDING_OWNERSHIP_INST(SelectValue)
FORWARDING_OWNERSHIP_INST(Struct)
FORWARDING_OWNERSHIP_INST(StructExtract)
FORWARDING_OWNERSHIP_INST(Tuple)
FORWARDING_OWNERSHIP_INST(TupleExtract)
FORWARDING_OWNERSHIP_INST(UncheckedEnumData)
FORWARDING_OWNERSHIP_INST(UncheckedRefCast)
FORWARDING_OWNERSHIP_INST(UnconditionalCheckedCast)
FORWARDING_OWNERSHIP_INST(Upcast)
#undef FORWARDING_OWNERSHIP_INST

ValueOwnershipKind ValueOwnershipKindVisitor::visitSILUndef(SILUndef *Arg) {
  return ValueOwnershipKind::Any;
}

ValueOwnershipKind
ValueOwnershipKindVisitor::visitPHISILArgument(SILArgument *Arg) {
  llvm::SmallVector<SILValue, 8> IncomingArgs;
  // If we can not find IncomingArgs, just return None.
  if (!Arg->getIncomingValues(IncomingArgs)) {
    return ValueOwnershipKind::None;
  }

  if (IncomingArgs.empty())
    llvm_unreachable("Must have incoming args at this point");

  llvm::Optional<ValueOwnershipKind> Result =
      IncomingArgs[0]->getOwnershipKind();
  for (SILValue V : ArrayRef<SILValue>(IncomingArgs).slice(1)) {
    Result = ValueOwnershipKindMerge(Result, V->getOwnershipKind());
  }

  return Result.getValue();
}

ValueOwnershipKind
ValueOwnershipKindVisitor::visitSILArgument(SILArgument *Arg) {
  // If we have a PHI node, we need to look at our predecessors.
  if (!Arg->isFunctionArg()) {
    return visitPHISILArgument(Arg);
  }

  // Otherwise, we need to discover our ownership kind from our function
  // signature.
  switch (Arg->getArgumentConvention()) {
  // We do not handle these for now.
  case SILArgumentConvention::Indirect_In:
  case SILArgumentConvention::Indirect_In_Guaranteed:
  case SILArgumentConvention::Indirect_Inout:
  case SILArgumentConvention::Indirect_InoutAliasable:
  case SILArgumentConvention::Indirect_Out:
    return ValueOwnershipKind::None;
  case SILArgumentConvention::Direct_Owned:
    return ValueOwnershipKind::Owned;
  case SILArgumentConvention::Direct_Unowned:
    if (Arg->getType().isTrivial(Arg->getModule()))
      return ValueOwnershipKind::Trivial;
    return ValueOwnershipKind::Unowned;
  case SILArgumentConvention::Direct_Guaranteed:
    return ValueOwnershipKind::Guaranteed;
  case SILArgumentConvention::Direct_Deallocating:
    llvm_unreachable("No ownership associated with deallocating");
  }
}

ValueOwnershipKind
ValueOwnershipKindVisitor::visitMarkDependenceInst(MarkDependenceInst *MDI) {
  return MDI->getValue()->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindVisitor::visitApplyInst(ApplyInst *AI) {
  // Without a result, our apply must have a trivial result.
  auto Result = AI->getSingleResult();
  if (!Result)
    return ValueOwnershipKind::Trivial;

  switch (Result.getValue().getConvention()) {
  case ResultConvention::Indirect:
    return ValueOwnershipKind::None;
  case ResultConvention::Autoreleased:
  case ResultConvention::Owned:
    return ValueOwnershipKind::Owned;
  case ResultConvention::Unowned:
  case ResultConvention::UnownedInnerPointer:
    return ValueOwnershipKind::Unowned;
  }
}

ValueOwnershipKind
ValueOwnershipKindVisitor::visitBuiltinInst(BuiltinInst *BI) {
  // For now, just conservatively say builtins are None.
  return ValueOwnershipKind::None;
#if 0
  if (auto Intrinsic = BI->getIntrinsicID()) {
    visitIntrinsicInst(BI, Intrinsic.getValue());
  }
  BuiltinValueKind Kind = BI->getBuiltinKind().getValue();
  switch (Kind) {
#define BUILTIN_CAST_OPERATION(Id, Name, Attrs)                                \
  case BuiltinValueKind::Id:                                                   \
    return ValueOwnershipKind::Trivial;
#define BUILTIN_CAST_OR_BITCAST_OPERATION(Id, Name, Attrs)                     \
  case BuiltinValueKind::Id:                                                   \
    return ValueOwnershipKind::Trivial;
#define BUILTIN_BINARY_OPERATION(Id, Name, Attrs, Overload)                    \
  case BuiltinValueKind::Id:                                                   \
    return ValueOwnershipKind::Trivial;
#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(Id, Name, UncheckedID, Attrs,   \
                                               Overload)                       \
  case BuiltinValueKind::Id:                                                   \
    return ValueOwnershipKind::Trivial;
#define BUILTIN_UNARY_OPERATION(Id, Name, Attrs, Overload)                     \
  case BuiltinValueKind::Id:                                                   \
    return ValueOwnershipKind::Trivial;
#define BUILTIN_BINARY_PREDICATE(Id, Name, Attrs, Overload)                    \
  case BuiltinValueKind::Id:                                                   \
    return ValueOwnershipKind::Trivial;
#include "swift/AST/Builtins.def"

#define NO_OWNERSHIP_BUILTIN(Name)                                             \
  case BuiltinValueKind::Id:                                                   \
    llvm_unreachable("Builtin " #Name " does not have any ownership");
    NO_OWNERSHIP_BUILTIN(Retain)
    NO_OWNERSHIP_BUILTIN(Release)
    NO_OWNERSHIP_BUILTIN(Autorelease)
    NO_OWNERSHIP_BUILTIN(Unpin)
    NO_OWNERSHIP_BUILTIN(Destroy)
    NO_OWNERSHIP_BUILTIN(Assign)
    NO_OWNERSHIP_BUILTIN(Init)
    NO_OWNERSHIP_BUILTIN(CondFail)
    NO_OWNERSHIP_BUILTIN(FixLifetime)
    NO_OWNERSHIP_BUILTIN(BindMemory)
    NO_OWNERSHIP_BUILTIN(WillThrow)
    NO_OWNERSHIP_BUILTIN(UnexpectedError)
    NO_OWNERSHIP_BUILTIN(ErrorInMain)
    NO_OWNERSHIP_BUILTIN(Fence)
    NO_OWNERSHIP_BUILTIN(OnFastPath)
    NO_OWNERSHIP_BUILTIN(AtomicStore)
#undef NO_OWNERSHIP_BUILTIN

#define CONSTANT_OWNERSHIP_BUILTIN(Name, Ownership)                            \
  case BuiltinValueKind::Id:                                                   \
    return ValueOwnershipKind::Ownership;
    CONSTANT_OWNERSHIP_BUILTIN(tryPin, Owned)
    CONSTANT_OWNERSHIP_BUILTIN(Load, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(LoadRaw, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(Take, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(CastToUnknownObject, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(CastFromUnknownObject, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(CastToNativeObject, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(CastToBridgeObject, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(CastReferenceFromBridgeObject, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(CastBitPatternFromBridgeObject, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(BridgeToRawPointer, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(ReinterpretCast, Unowned)
    CONSTANT_OWNERSHIP_BUILTIN(AddressOf, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(GepRaw, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(Gep, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(GetTailAddr, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(IsUnique, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(IsUniqueOrPinned, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(IsUnique_native, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(IsUniqueOrPinned_native, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(AllocWithTailElems, Owned)
    CONSTANT_OWNERSHIP_BUILTIN(ProjectTailElims, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(IsOptionalType, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(Sizeof, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(Strideof, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(IsPOD, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(Alignof, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(AllocRaw, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(CmpXChg, Trivial)
    CONSTANT_OWNERSHIP_BUILTIN(AtomicLoad, Unowned)
#undef CONSTANT_OWNERSHIP_BUILTIN

#define FORWARDING_OWNERSHIP_BUILTIN(Name)                                     \
  case BuiltinValueKind::Id:                                                   \
    return visitForwardingInst(BI);
    FORWARDING_OWNERSHIP_BUILTIN(CastReference)
#undef FORWARDING_OWNERSHIP_BUILTIN
  default:
    llvm_unreachable("Unimplemented builtin");
  }
#endif
}

ValueOwnershipKind ValueOwnershipKindVisitor::visitLoadInst(LoadInst *LI) {
  switch (LI->getOwnershipQualifier()) {
  case LoadOwnershipQualifier::Take:
  case LoadOwnershipQualifier::Copy:
    return ValueOwnershipKind::Owned;
  case LoadOwnershipQualifier::Unqualified:
    return ValueOwnershipKind::Unowned;
  case LoadOwnershipQualifier::Trivial:
    return ValueOwnershipKind::Trivial;
  }
}

ValueOwnershipKind ValueBase::getOwnershipKind() const {
  return ValueOwnershipKindVisitor().visit(const_cast<ValueBase *>(this));
}

//===----------------------------------------------------------------------===//
//                                 Utilities
//===----------------------------------------------------------------------===//

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &OS,
                                     SILInstruction::MemoryBehavior B) {
  switch (B) {
    case SILInstruction::MemoryBehavior::None:
      return OS << "None";
    case SILInstruction::MemoryBehavior::MayRead:
      return OS << "MayRead";
    case SILInstruction::MemoryBehavior::MayWrite:
      return OS << "MayWrite";
    case SILInstruction::MemoryBehavior::MayReadWrite:
      return OS << "MayReadWrite";
    case SILInstruction::MemoryBehavior::MayHaveSideEffects:
      return OS << "MayHaveSideEffects";
  }
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &OS,
                                     SILInstruction::ReleasingBehavior B) {
  switch (B) {
  case SILInstruction::ReleasingBehavior::DoesNotRelease:
    return OS << "DoesNotRelease";
  case SILInstruction::ReleasingBehavior::MayRelease:
    return OS << "MayRelease";
  }
}
