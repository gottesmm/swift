//===--- MoveOnlyUtils.cpp ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "swift/AST/AccessScope.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILArgumentConvention.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#include "MoveOnlyDiagnostics.h"
#include "MoveOnlyUtils.h"

using namespace swift;
using namespace swift::siloptimizer;

//===----------------------------------------------------------------------===//
//                        MARK: Missed Copy Diagnostic
//===----------------------------------------------------------------------===//

/// A small diagnostic helper that causes us to emit a diagnostic error upon any
/// copies we did not eliminate and ask the user for a test case.
void swift::siloptimizer::emitCheckerMissedCopyOfNonCopyableTypeErrors(
    SILFunction *fn, DiagnosticEmitter &diagnosticEmitter) {
  for (auto &block : *fn) {
    for (auto &inst : block) {
      if (auto *cvi = dyn_cast<CopyValueInst>(&inst)) {
        if (cvi->getOperand()->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs()
                     << "Emitting missed copy error for: " << *cvi);
          diagnosticEmitter.emitCheckedMissedCopyError(cvi);
        }
        continue;
      }

      if (auto *li = dyn_cast<LoadInst>(&inst)) {
        if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy &&
            li->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs() << "Emitting missed copy error for: " << *li);
          diagnosticEmitter.emitCheckedMissedCopyError(li);
        }
        continue;
      }

      if (auto *copyAddr = dyn_cast<CopyAddrInst>(&inst)) {
        if (!copyAddr->isTakeOfSrc() &&
            copyAddr->getSrc()->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs()
                     << "Emitting missed copy error for: " << *copyAddr);
          diagnosticEmitter.emitCheckedMissedCopyError(copyAddr);
        }
        continue;
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                  MARK: Cleanup After Emitting Diagnostic
//===----------------------------------------------------------------------===//

bool swift::siloptimizer::cleanupNonCopyableCopiesAfterEmittingDiagnostic(
    SILFunction *fn) {
  bool changed = false;
  for (auto &block : *fn) {
    for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
      auto *inst = &*ii;
      ++ii;

      // Convert load [copy] *MoveOnly -> load_borrow + explicit_copy_value.
      if (auto *li = dyn_cast<LoadInst>(inst)) {
        if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
          if (!li->getType().isMoveOnly())
            continue;

          SILBuilderWithScope builder(li);
          auto *lbi = builder.createLoadBorrow(li->getLoc(), li->getOperand());
          auto *cvi = builder.createExplicitCopyValue(li->getLoc(), lbi);
          builder.createEndBorrow(li->getLoc(), lbi);
          li->replaceAllUsesWith(cvi);
          li->eraseFromParent();
          changed = true;
        }
      }

      // Convert copy_addr !take MoveOnly ... -> explicit_copy_addr ...same...
      // so we don't error.
      if (auto *copyAddr = dyn_cast<CopyAddrInst>(inst)) {
        if (!copyAddr->isTakeOfSrc()) {
          if (!copyAddr->getSrc()->getType().isMoveOnly())
            continue;

          SILBuilderWithScope builder(copyAddr);
          builder.createExplicitCopyAddr(
              copyAddr->getLoc(), copyAddr->getSrc(), copyAddr->getDest(),
              IsTake_t(copyAddr->isTakeOfSrc()),
              IsInitialization_t(copyAddr->isInitializationOfDest()));
          copyAddr->eraseFromParent();
          changed = true;
        }
      }

      // Convert any copy_value of MoveOnly type -> explicit_copy_value.
      if (auto *cvi = dyn_cast<CopyValueInst>(inst)) {
        if (!cvi->getOperand()->getType().isMoveOnly())
          continue;

        SILBuilderWithScope b(cvi);
        auto *expCopy =
            b.createExplicitCopyValue(cvi->getLoc(), cvi->getOperand());
        cvi->replaceAllUsesWith(expCopy);
        cvi->eraseFromParent();
        changed = true;
        continue;
      }

      if (auto *mmci = dyn_cast<MarkMustCheckInst>(inst)) {
        mmci->replaceAllUsesWith(mmci->getOperand());
        mmci->eraseFromParent();
        changed = true;
        continue;
      }
    }
  }

  return changed;
}

//===----------------------------------------------------------------------===//
//                          MARK: Memory Operations
//===----------------------------------------------------------------------===//

bool moveonlyutils::memInstMustConsume(Operand *memOper) {
  SILValue address = memOper->get();

  SILInstruction *memInst = memOper->getUser();

  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return (CAI->getSrc() == address && CAI->isTakeOfSrc()) ||
           (CAI->getDest() == address && !CAI->isInitializationOfDest());
  }
  case SILInstructionKind::ExplicitCopyAddrInst: {
    auto *CAI = cast<ExplicitCopyAddrInst>(memInst);
    return (CAI->getSrc() == address && CAI->isTakeOfSrc()) ||
           (CAI->getDest() == address && !CAI->isInitializationOfDest());
  }
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::ApplyInst: {
    FullApplySite applySite(memInst);
    return applySite.getArgumentOperandConvention(*memOper).isOwnedConvention();
  }
  case SILInstructionKind::PartialApplyInst: {
    // If we are on the stack or have an inout convention, we do not
    // consume. Otherwise, we do.
    auto *pai = cast<PartialApplyInst>(memInst);
    if (pai->isOnStack())
      return false;
    ApplySite applySite(pai);
    auto convention = applySite.getArgumentConvention(*memOper);
    return !convention.isInoutConvention();
  }
  case SILInstructionKind::DestroyAddrInst:
    return true;
  case SILInstructionKind::LoadInst:
    return cast<LoadInst>(memInst)->getOwnershipQualifier() ==
           LoadOwnershipQualifier::Take;
  }
}

bool moveonlyutils::memInstMustReinitialize(Operand *memOper) {
  SILValue address = memOper->get();

  SILInstruction *memInst = memOper->getUser();

  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return CAI->getDest() == address && !CAI->isInitializationOfDest();
  }
  case SILInstructionKind::ExplicitCopyAddrInst: {
    auto *CAI = cast<ExplicitCopyAddrInst>(memInst);
    return CAI->getDest() == address && !CAI->isInitializationOfDest();
  }
  case SILInstructionKind::YieldInst: {
    auto *yield = cast<YieldInst>(memInst);
    return yield->getYieldInfoForOperand(*memOper).isIndirectInOut();
  }
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::ApplyInst: {
    FullApplySite applySite(memInst);
    return applySite.getArgumentOperandConvention(*memOper).isInoutConvention();
  }
  case SILInstructionKind::StoreInst:
    return cast<StoreInst>(memInst)->getOwnershipQualifier() ==
           StoreOwnershipQualifier::Assign;

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  case SILInstructionKind::Store##Name##Inst:                                  \
    return !cast<Store##Name##Inst>(memInst)->isInitializationOfDest();
#include "swift/AST/ReferenceStorage.def"
  }
}

bool moveonlyutils::memInstMustInitialize(Operand *memOper) {
  SILValue address = memOper->get();

  SILInstruction *memInst = memOper->getUser();

  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return CAI->getDest() == address && CAI->isInitializationOfDest();
  }
  case SILInstructionKind::ExplicitCopyAddrInst: {
    auto *CAI = cast<ExplicitCopyAddrInst>(memInst);
    return CAI->getDest() == address && CAI->isInitializationOfDest();
  }
  case SILInstructionKind::MarkUnresolvedMoveAddrInst: {
    return cast<MarkUnresolvedMoveAddrInst>(memInst)->getDest() == address;
  }
  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::InjectEnumAddrInst:
    return true;

  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::ApplyInst: {
    FullApplySite applySite(memInst);
    return applySite.isIndirectResultOperand(*memOper);
  }
  case SILInstructionKind::StoreInst: {
    auto qual = cast<StoreInst>(memInst)->getOwnershipQualifier();
    return qual == StoreOwnershipQualifier::Init ||
           qual == StoreOwnershipQualifier::Trivial;
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  case SILInstructionKind::Store##Name##Inst:                                  \
    return cast<Store##Name##Inst>(memInst)->isInitializationOfDest();
#include "swift/AST/ReferenceStorage.def"
  }
}

bool moveonlyutils::memInstOnlyRequiresLiveness(Operand *op) {
  auto *user = op->getUser();

  if (isa<LoadBorrowInst>(user))
    return true;

  if (auto fas = FullApplySite::isa(user)) {
    switch (fas.getArgumentConvention(*op)) {
    case SILArgumentConvention::Indirect_In_Guaranteed:
      return true;
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_InoutAliasable:
    case SILArgumentConvention::Indirect_In:
    case SILArgumentConvention::Indirect_Out:
    case SILArgumentConvention::Direct_Unowned:
    case SILArgumentConvention::Direct_Owned:
    case SILArgumentConvention::Direct_Guaranteed:
    case SILArgumentConvention::Pack_Inout:
    case SILArgumentConvention::Pack_Owned:
    case SILArgumentConvention::Pack_Guaranteed:
    case SILArgumentConvention::Pack_Out:
      return false;
    }
  }

  if (auto *yi = dyn_cast<YieldInst>(user)) {
    return yi->getYieldInfoForOperand(*op).isGuaranteed();
  }

  // Otherwise be conservative and delegate to may write to memory.
  return user->mayWriteToMemory();
}
