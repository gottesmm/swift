//===--- PMOMemoryUseCollector.cpp - Memory use analysis for PMO ----------===//
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

#define DEBUG_TYPE "definite-init"
#include "PMOMemoryUseCollector.h"
#include "swift/AST/Expr.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                  PMOMemoryObjectInfo Implementation
//===----------------------------------------------------------------------===//

static unsigned getElementCountRec(SILModule &Module, SILType T) {
  // If this is a tuple, it is always recursively flattened.
  if (CanTupleType TT = T.getAs<TupleType>()) {
    unsigned NumElements = 0;
    for (unsigned i = 0, e = TT->getNumElements(); i < e; i++)
      NumElements += getElementCountRec(Module, T.getTupleElementType(i));
    return NumElements;
  }

  // Otherwise, it is a single element.
  return 1;
}

PMOMemoryObjectInfo::PMOMemoryObjectInfo(SingleValueInstruction *MI) {
  auto &Module = MI->getModule();

  MemoryInst = MI;
  // Compute the type of the memory object.
  if (auto *ABI = dyn_cast<AllocBoxInst>(MemoryInst)) {
    assert(ABI->getBoxType()->getLayout()->getFields().size() == 1 &&
           "analyzing multi-field boxes not implemented");
    MemorySILType = ABI->getBoxType()->getFieldType(Module, 0);
  } else if (auto *ASI = dyn_cast<AllocStackInst>(MemoryInst)) {
    MemorySILType = ASI->getElementType();
  } else {
    llvm_unreachable(
        "Predictable Mem Opts should only be analyzing alloc_box/alloc_stack");
  }

  // Break down the initializer.
  NumElements = getElementCountRec(Module, MemorySILType);
}

SILInstruction *PMOMemoryObjectInfo::getFunctionEntryPoint() const {
  return &*getFunction().begin()->begin();
}

/// Given a symbolic element number, return the type of the element.
static SILType getElementTypeRec(SILModule &Module, SILType T, unsigned EltNo) {
  // If this is a tuple type, walk into it.
  if (CanTupleType TT = T.getAs<TupleType>()) {
    for (unsigned i = 0, e = TT->getNumElements(); i < e; i++) {
      auto FieldType = T.getTupleElementType(i);
      unsigned NumFieldElements = getElementCountRec(Module, FieldType);
      if (EltNo < NumFieldElements)
        return getElementTypeRec(Module, FieldType, EltNo);
      EltNo -= NumFieldElements;
    }
    // This can only happen if we look at a symbolic element number of an empty
    // tuple.
    llvm::report_fatal_error("invalid element number");
  }

  // Otherwise, it is a leaf element.
  assert(EltNo == 0);
  return T;
}

/// getElementTypeRec - Return the swift type of the specified element.
SILType PMOMemoryObjectInfo::getElementType(unsigned EltNo) const {
  auto &Module = MemoryInst->getModule();
  return getElementTypeRec(Module, MemorySILType, EltNo);
}

/// Push the symbolic path name to the specified element number onto the
/// specified std::string.
static void getPathStringToElementRec(SILModule &Module, SILType T,
                                      unsigned EltNo, std::string &Result) {
  if (CanTupleType TT = T.getAs<TupleType>()) {
    unsigned FieldNo = 0;
    for (unsigned i = 0, e = TT->getNumElements(); i < e; i++) {
      auto Field = TT->getElement(i);
      SILType FieldTy = T.getTupleElementType(i);
      unsigned NumFieldElements = getElementCountRec(Module, FieldTy);

      if (EltNo < NumFieldElements) {
        Result += '.';
        if (Field.hasName())
          Result += Field.getName().str();
        else
          Result += llvm::utostr(FieldNo);
        return getPathStringToElementRec(Module, FieldTy, EltNo, Result);
      }

      EltNo -= NumFieldElements;

      ++FieldNo;
    }
    llvm_unreachable("Element number is out of range for this type!");
  }

  // Otherwise, there are no subelements.
  assert(EltNo == 0 && "Element count problem");
}

ValueDecl *
PMOMemoryObjectInfo::getPathStringToElement(unsigned Element,
                                            std::string &Result) const {
  auto &Module = MemoryInst->getModule();

  if (auto *VD = dyn_cast_or_null<ValueDecl>(getLoc().getAsASTNode<Decl>()))
    Result = VD->getBaseName().userFacingName();
  else
    Result = "<unknown>";

  // Get the path through a tuple, if relevant.
  getPathStringToElementRec(Module, MemorySILType, Element, Result);

  // Otherwise, we can't.
  return nullptr;
}

/// If the specified value is a 'let' property in an initializer, return true.
bool PMOMemoryObjectInfo::isElementLetProperty(unsigned Element) const {
  // If we aren't representing 'self' in a non-delegating initializer, then we
  // can't have 'let' properties.
  return IsLet;
}

//===----------------------------------------------------------------------===//
//                        PMOMemoryUse Implementation
//===----------------------------------------------------------------------===//

/// onlyTouchesTrivialElements - Return true if all of the accessed elements
/// have trivial type.
bool PMOMemoryUse::onlyTouchesTrivialElements(
    const PMOMemoryObjectInfo &MI) const {
  auto &Module = Inst->getModule();

  for (unsigned i = FirstElement, e = i + NumElements; i != e; ++i) {
    // Skip 'super.init' bit
    if (i == MI.getNumMemoryElements())
      return false;

    auto EltTy = MI.getElementType(i);
    if (!EltTy.isTrivial(Module))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
//                     ElementUseCollector Implementation
//===----------------------------------------------------------------------===//

namespace {

class ElementUseCollector {
  SILModule &Module;
  const PMOMemoryObjectInfo &TheMemory;
  SmallVectorImpl<PMOMemoryUse> &Uses;
  SmallVectorImpl<SILInstruction *> &Releases;

  /// When walking the use list, if we index into a struct element, keep track
  /// of this, so that any indexes into tuple subelements don't affect the
  /// element we attribute an access to.
  bool InStructSubElement = false;

  /// When walking the use list, if we index into an enum slice, keep track
  /// of this.
  bool InEnumSubElement = false;

public:
  ElementUseCollector(const PMOMemoryObjectInfo &TheMemory,
                      SmallVectorImpl<PMOMemoryUse> &Uses,
                      SmallVectorImpl<SILInstruction *> &Releases)
      : Module(TheMemory.MemoryInst->getModule()), TheMemory(TheMemory),
        Uses(Uses), Releases(Releases) {}

  /// This is the main entry point for the use walker.  It collects uses from
  /// the address and the refcount result of the allocation.
  LLVM_NODISCARD bool collectFrom();

private:
  LLVM_NODISCARD bool collectUses(SILValue Pointer, unsigned BaseEltNo);
  LLVM_NODISCARD bool collectContainerUses(AllocBoxInst *ABI);
  void addElementUses(unsigned BaseEltNo, SILType UseTy, SILInstruction *User,
                      PMOUseKind Kind);
  LLVM_NODISCARD bool collectTupleElementUses(TupleElementAddrInst *TEAI,
                                              unsigned BaseEltNo);
  LLVM_NODISCARD bool collectStructElementUses(StructElementAddrInst *SEAI,
                                               unsigned BaseEltNo);
};

} // end anonymous namespace

bool ElementUseCollector::collectFrom() {
  bool shouldOptimize = false;

  if (auto *ABI = TheMemory.getContainer()) {
    shouldOptimize = collectContainerUses(ABI);
  } else {
    shouldOptimize = collectUses(TheMemory.getAddress(), 0);
  }

  if (!shouldOptimize)
    return false;

  // Collect information about the retain count result as well.
  for (auto UI : TheMemory.MemoryInst->getUses()) {
    auto *User = UI->getUser();

    // If this is a release or dealloc_stack, then remember it as such.
    if (isa<StrongReleaseInst>(User) || isa<DeallocStackInst>(User) ||
        isa<DeallocBoxInst>(User)) {
      Releases.push_back(User);
    }
  }

  return true;
}

/// addElementUses - An operation (e.g. load, store, inout use, etc) on a value
/// acts on all of the aggregate elements in that value.  For example, a load
/// of $*(Int,Int) is a use of both Int elements of the tuple.  This is a helper
/// to keep the Uses data structure up to date for aggregate uses.
void ElementUseCollector::addElementUses(unsigned BaseEltNo, SILType UseTy,
                                         SILInstruction *User,
                                         PMOUseKind Kind) {
  // If we're in a subelement of a struct or enum, just mark the struct, not
  // things that come after it in a parent tuple.
  unsigned NumElements = 1;
  if (TheMemory.NumElements != 1 && !InStructSubElement && !InEnumSubElement)
    NumElements = getElementCountRec(Module, UseTy);

  Uses.push_back(PMOMemoryUse(User, Kind, BaseEltNo, NumElements));
}

/// Given a tuple_element_addr or struct_element_addr, compute the new
/// BaseEltNo implicit in the selected member, and recursively add uses of
/// the instruction.
bool ElementUseCollector::collectTupleElementUses(TupleElementAddrInst *TEAI,
                                                  unsigned BaseEltNo) {

  // If we're walking into a tuple within a struct or enum, don't adjust the
  // BaseElt.  The uses hanging off the tuple_element_addr are going to be
  // counted as uses of the struct or enum itself.
  if (InStructSubElement || InEnumSubElement)
    return collectUses(TEAI, BaseEltNo);

  // tuple_element_addr P, 42 indexes into the current tuple element.
  // Recursively process its uses with the adjusted element number.
  unsigned FieldNo = TEAI->getFieldNo();
  auto T = TEAI->getOperand()->getType();
  if (T.is<TupleType>()) {
    for (unsigned i = 0; i != FieldNo; ++i) {
      SILType EltTy = T.getTupleElementType(i);
      BaseEltNo += getElementCountRec(Module, EltTy);
    }
  }

  return collectUses(TEAI, BaseEltNo);
}

bool ElementUseCollector::collectStructElementUses(StructElementAddrInst *SEAI,
                                                   unsigned BaseEltNo) {
  // Generally, we set the "InStructSubElement" flag and recursively process
  // the uses so that we know that we're looking at something within the
  // current element.
  llvm::SaveAndRestore<bool> X(InStructSubElement, true);
  return collectUses(SEAI, BaseEltNo);
}

bool ElementUseCollector::collectContainerUses(AllocBoxInst *ABI) {
  for (Operand *UI : ABI->getUses()) {
    auto *User = UI->getUser();

    // Deallocations and retain/release don't affect the value directly.
    if (isa<DeallocBoxInst>(User))
      continue;
    if (isa<StrongRetainInst>(User))
      continue;
    if (isa<StrongReleaseInst>(User))
      continue;

    if (auto project = dyn_cast<ProjectBoxInst>(User)) {
      if (!collectUses(project, project->getFieldIndex()))
        return false;
      continue;
    }

    // Other uses of the container are considered escapes of the values.
    for (unsigned field :
         indices(ABI->getBoxType()->getLayout()->getFields())) {
      addElementUses(field,
                     ABI->getBoxType()->getFieldType(ABI->getModule(), field),
                     User, PMOUseKind::Escape);
    }
  }

  return true;
}

// Returns true when the instruction represents added instrumentation for
/// run-time sanitizers.
static bool isSanitizerInstrumentation(SILInstruction *Instruction,
                                       ASTContext &Ctx) {
  auto *BI = dyn_cast<BuiltinInst>(Instruction);
  if (!BI)
    return false;

  Identifier Name = BI->getName();
  if (Name == Ctx.getIdentifier("tsanInoutAccess"))
    return true;

  return false;
}

bool ElementUseCollector::collectUses(SILValue Pointer, unsigned BaseEltNo) {
  assert(Pointer->getType().isAddress() &&
         "Walked through the pointer to the value?");
  SILType PointeeType = Pointer->getType().getObjectType();

  for (auto *UI : Pointer->getUses()) {
    auto *User = UI->getUser();

    // struct_element_addr P, #field indexes into the current element.
    if (auto *SEAI = dyn_cast<StructElementAddrInst>(User)) {
      if (!collectStructElementUses(SEAI, BaseEltNo))
        return false;
      continue;
    }

    // Instructions that compute a subelement are handled by a helper.
    if (auto *TEAI = dyn_cast<TupleElementAddrInst>(User)) {
      if (!collectTupleElementUses(TEAI, BaseEltNo))
        return false;
      continue;
    }

    // Look through begin_access.
    if (auto I = dyn_cast<BeginAccessInst>(User)) {
      if (!collectUses(I, BaseEltNo))
        return false;
      continue;
    }

    // Ignore end_access.
    if (isa<EndAccessInst>(User)) {
      continue;
    }

    // Loads are a use of the value.
    if (isa<LoadInst>(User)) {
      assert(!PointeeType.is<TupleType>() && "Should have been canonicalized away by the tuple memory operation canonicalizer");
      addElementUses(BaseEltNo, PointeeType, User, PMOUseKind::Load);
      continue;
    }

    if (isa<LoadWeakInst>(User)) {
      Uses.push_back(PMOMemoryUse(User, PMOUseKind::Load, BaseEltNo, 1));
      continue;
    }

    // Stores *to* the allocation are writes.
    if (isa<StoreInst>(User) && UI->getOperandNumber() == 1) {
      assert(!PointeeType.is<TupleType>() && "Should have been canonicalized away by the tuple memory operation canonicalizer");

      // Coming out of SILGen, we assume that raw stores are initializations,
      // unless they have trivial type (which we classify as InitOrAssign).
      PMOUseKind Kind;
      if (InStructSubElement)
        Kind = PMOUseKind::PartialStore;
      else if (PointeeType.isTrivial(User->getModule()))
        Kind = PMOUseKind::InitOrAssign;
      else
        Kind = PMOUseKind::Initialization;

      addElementUses(BaseEltNo, PointeeType, User, Kind);
      continue;
    }

    if (auto *SWI = dyn_cast<StoreWeakInst>(User)) {
      if (UI->getOperandNumber() == 1) {
        PMOUseKind Kind;
        if (InStructSubElement)
          Kind = PMOUseKind::PartialStore;
        else if (SWI->isInitializationOfDest())
          Kind = PMOUseKind::Initialization;
        else
          Kind = PMOUseKind::Assign;
        Uses.push_back(PMOMemoryUse(User, Kind, BaseEltNo, 1));
        continue;
      }
    }

    if (auto *SUI = dyn_cast<StoreUnownedInst>(User)) {
      if (UI->getOperandNumber() == 1) {
        PMOUseKind Kind;
        if (InStructSubElement)
          Kind = PMOUseKind::PartialStore;
        else if (SUI->isInitializationOfDest())
          Kind = PMOUseKind::Initialization;
        else
          Kind = PMOUseKind::Assign;
        Uses.push_back(PMOMemoryUse(User, Kind, BaseEltNo, 1));
        continue;
      }
    }

    if (auto *CAI = dyn_cast<CopyAddrInst>(User)) {
      // If this is a copy of a tuple, we should have scalarize it so that we don't
      // have an access that crosses elements.
      assert(!PointeeType.is<TupleType>() && "Should have been canonicalized away by the tuple memory operation canonicalizer");

      // If this is the source of the copy_addr, then this is a load.  If it is
      // the destination, then this is an unknown assignment.  Note that we'll
      // revisit this instruction and add it to Uses twice if it is both a load
      // and store to the same aggregate.
      PMOUseKind Kind;
      if (UI->getOperandNumber() == 0)
        Kind = PMOUseKind::Load;
      else if (InStructSubElement)
        Kind = PMOUseKind::PartialStore;
      else if (CAI->isInitializationOfDest())
        Kind = PMOUseKind::Initialization;
      else
        Kind = PMOUseKind::Assign;

      addElementUses(BaseEltNo, PointeeType, User, Kind);
      continue;
    }

    // The apply instruction does not capture the pointer when it is passed
    // through 'inout' arguments or for indirect returns.  InOut arguments are
    // treated as uses and may-store's, but an indirect return is treated as a
    // full store.
    //
    // Note that partial_apply instructions always close over their argument.
    //
    if (auto *Apply = dyn_cast<ApplyInst>(User)) {
      auto substConv = Apply->getSubstCalleeConv();
      unsigned ArgumentNumber = UI->getOperandNumber() - 1;

      // If this is an out-parameter, it is like a store.
      unsigned NumIndirectResults = substConv.getNumIndirectSILResults();
      if (ArgumentNumber < NumIndirectResults) {
        // We do not support initializing sub members. This is an old
        // restriction from when this code was used by Definite
        // Initialization. With proper code review, we can remove this, but for
        // now, lets be conservative.
        if (InStructSubElement) {
          return false;
        }
        addElementUses(BaseEltNo, PointeeType, User,
                       PMOUseKind::Initialization);
        continue;

        // Otherwise, adjust the argument index.
      } else {
        ArgumentNumber -= NumIndirectResults;
      }

      auto ParamConvention =
          substConv.getParameters()[ArgumentNumber].getConvention();

      switch (ParamConvention) {
      case ParameterConvention::Direct_Owned:
      case ParameterConvention::Direct_Unowned:
      case ParameterConvention::Direct_Guaranteed:
        llvm_unreachable("address value passed to indirect parameter");

      // If this is an in-parameter, it is like a load.
      case ParameterConvention::Indirect_In:
      case ParameterConvention::Indirect_In_Constant:
      case ParameterConvention::Indirect_In_Guaranteed:
        addElementUses(BaseEltNo, PointeeType, User, PMOUseKind::IndirectIn);
        continue;

      // If this is an @inout parameter, it is like both a load and store.
      case ParameterConvention::Indirect_Inout:
      case ParameterConvention::Indirect_InoutAliasable: {
        // If we're in the initializer for a struct, and this is a call to a
        // mutating method, we model that as an escape of self.  If an
        // individual sub-member is passed as inout, then we model that as an
        // inout use.
        addElementUses(BaseEltNo, PointeeType, User, PMOUseKind::InOutUse);
        continue;
      }
      }
      llvm_unreachable("bad parameter convention");
    }

    // init_enum_data_addr is treated like a tuple_element_addr or other
    // instruction that is looking into the memory object (i.e., the memory
    // object needs to be explicitly initialized by a copy_addr or some other
    // use of the projected address).
    if (auto I = dyn_cast<InitEnumDataAddrInst>(User)) {
      // If we are in a struct already, bail. With proper analysis, we should be
      // able to do this optimization.
      if (InStructSubElement) {
        return false;
      }

      // Keep track of the fact that we're inside of an enum.  This informs our
      // recursion that tuple stores are not scalarized outside, and that stores
      // should not be treated as partial stores.
      llvm::SaveAndRestore<bool> X(InEnumSubElement, true);
      if (!collectUses(I, BaseEltNo))
        return false;
      continue;
    }

    // init_existential_addr is modeled as an initialization store.
    if (isa<InitExistentialAddrInst>(User)) {
      // init_existential_addr should not apply to struct subelements.
      if (InStructSubElement) {
        return false;
      }
      Uses.push_back(
          PMOMemoryUse(User, PMOUseKind::Initialization, BaseEltNo, 1));
      continue;
    }

    // inject_enum_addr is modeled as an initialization store.
    if (isa<InjectEnumAddrInst>(User)) {
      // inject_enum_addr the subelement of a struct unless in a ctor.
      if (InStructSubElement) {
        return false;
      }
      Uses.push_back(
          PMOMemoryUse(User, PMOUseKind::Initialization, BaseEltNo, 1));
      continue;
    }

    // open_existential_addr is a use of the protocol value,
    // so it is modeled as a load.
    if (isa<OpenExistentialAddrInst>(User)) {
      Uses.push_back(PMOMemoryUse(User, PMOUseKind::Load, BaseEltNo, 1));
      // TODO: Is it safe to ignore all uses of the open_existential_addr?
      continue;
    }

    // We model destroy_addr as a release of the entire value.
    if (isa<DestroyAddrInst>(User)) {
      Releases.push_back(User);
      continue;
    }

    if (isa<DeallocStackInst>(User)) {
      continue;
    }

    // Sanitizer instrumentation is not user visible, so it should not
    // count as a use and must not affect compile-time diagnostics.
    if (isSanitizerInstrumentation(User, Module.getASTContext()))
      continue;

    // Otherwise, the use is something complicated, it escapes.
    addElementUses(BaseEltNo, PointeeType, User, PMOUseKind::Escape);
  }

  return true;
}

/// collectPMOElementUsesFrom - Analyze all uses of the specified allocation
/// instruction (alloc_box, alloc_stack or mark_uninitialized), classifying them
/// and storing the information found into the Uses and Releases lists.
bool swift::collectPMOElementUsesFrom(
    const PMOMemoryObjectInfo &MemoryInfo, SmallVectorImpl<PMOMemoryUse> &Uses,
    SmallVectorImpl<SILInstruction *> &Releases) {
  return ElementUseCollector(MemoryInfo, Uses, Releases).collectFrom();
}
