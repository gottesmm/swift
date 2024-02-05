//===--- VariableNameUtils.h ----------------------------------------------===//
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
///
/// Utilities for inferring the name of a value.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_VARIABLENAMEUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_VARIABLENAMEUTILS_H

#include "swift/SIL/ApplySite.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/StackList.h"

namespace swift {

class VariableNameInferrer {
  StackList<PointerUnion<SILInstruction *, SILValue>> variableNamePath;
  SmallString<64> &resultingString;

public:
  VariableNameInferrer(SILFunction *fn, SmallString<64> &resultingString)
      : variableNamePath(fn), resultingString(resultingString) {}

  /// Attempts to infer a name from just uses of \p searchValue.
  ///
  /// Returns true if we found a name.
  bool tryInferNameFromUses(SILValue searchValue) {
    auto *use = getAnyDebugUse(searchValue);
    if (!use)
      return false;

    auto debugVar = DebugVarCarryingInst(use->getUser());
    if (!debugVar)
      return false;

    assert(debugVar.getKind() == DebugVarCarryingInst::Kind::DebugValue);
    resultingString += debugVar.getName();
    return true;
  }

  /// See if \p searchValue or one of its defs (walking use->def) has a name
  /// that we can use.
  ///
  /// \p failInsteadOfEmittingUnknown set to true if we should return false
  /// rather than emitting unknown (and always succeeding).
  ///
  /// \returns true if we inferred anything. Returns false otherwise.
  bool inferByWalkingUsesToDefs(SILValue searchValue,
                                bool failInsteadOfEmittingUnknown = false) {
    while (searchValue) {
      if (auto *allocInst = dyn_cast<AllocationInst>(searchValue)) {
        // If the instruction itself doesn't carry any variable info, see
        // whether it's copied from another place that does.
        if (!allocInst->getDecl()) {
          if (auto copy = allocInst->getSingleUserOfType<CopyAddrInst>()) {
            if (copy->getDest() == allocInst && !copy->isTakeOfSrc() &&
                copy->isInitializationOfDest()) {
              searchValue = copy->getSrc();
              continue;
            }
          }
        }

        variableNamePath.push_back(allocInst);
        break;
      }

      if (auto *globalAddrInst = dyn_cast<GlobalAddrInst>(searchValue)) {
        variableNamePath.push_back(globalAddrInst);
        break;
      }

      if (auto *oeInst = dyn_cast<OpenExistentialAddrInst>(searchValue)) {
        searchValue = oeInst->getOperand();
        continue;
      }

      if (auto *rei = dyn_cast<RefElementAddrInst>(searchValue)) {
        variableNamePath.push_back(rei);
        searchValue = rei->getOperand();
        continue;
      }

      if (auto *fArg = dyn_cast<SILFunctionArgument>(searchValue)) {
        variableNamePath.push_back({fArg});
        break;
      }

      auto getNamePathComponentFromCallee =
          [&](FullApplySite call) -> std::optional<SILValue> {
        // Use the name of the property being accessed if we can get to it.
        if (isa<FunctionRefBaseInst>(call.getCallee()) ||
            isa<MethodInst>(call.getCallee())) {
          variableNamePath.push_back(
              call.getCallee()->getDefiningInstruction());
          // Try to name the base of the property if this is a method.
          if (call.getSubstCalleeType()->hasSelfParam()) {
            return call.getSelfArgument();
          }

          return SILValue();
        }
        return {};
      };

      // Read or modify accessor.
      if (auto bai = dyn_cast_or_null<BeginApplyInst>(
              searchValue->getDefiningInstruction())) {
        if (auto selfParam = getNamePathComponentFromCallee(bai)) {
          searchValue = *selfParam;
          continue;
        }
      }

      // Addressor accessor.
      if (auto ptrToAddr =
              dyn_cast<PointerToAddressInst>(stripAccessMarkers(searchValue))) {
        // The addressor can either produce the raw pointer itself or an
        // `UnsafePointer` stdlib type wrapping it.
        ApplyInst *addressorInvocation;
        if (auto structExtract =
                dyn_cast<StructExtractInst>(ptrToAddr->getOperand())) {
          addressorInvocation =
              dyn_cast<ApplyInst>(structExtract->getOperand());
        } else {
          addressorInvocation = dyn_cast<ApplyInst>(ptrToAddr->getOperand());
        }

        if (addressorInvocation) {
          if (auto selfParam =
                  getNamePathComponentFromCallee(addressorInvocation)) {
            searchValue = *selfParam;
            continue;
          }
        }
      }

      // If we do not do an exact match, see if we can find a debug_var inst. If
      // we do, we always break since we have a root value.
      if (auto *use = getAnyDebugUse(searchValue)) {
        if (auto debugVar = DebugVarCarryingInst(use->getUser())) {
          assert(debugVar.getKind() == DebugVarCarryingInst::Kind::DebugValue);
          variableNamePath.push_back(use->getUser());
          break;
        }
      }

      // Otherwise, try to see if we have a single value instruction we can look
      // through.
      if (isa<BeginBorrowInst>(searchValue) || isa<LoadInst>(searchValue) ||
          isa<LoadBorrowInst>(searchValue) ||
          isa<BeginAccessInst>(searchValue) ||
          isa<MarkUnresolvedNonCopyableValueInst>(searchValue) ||
          isa<ProjectBoxInst>(searchValue) || isa<CopyValueInst>(searchValue)) {
        searchValue = cast<SingleValueInstruction>(searchValue)->getOperand(0);
        continue;
      }

      // If we do not pattern match successfully, just set resulting string to
      // unknown and return early.
      if (failInsteadOfEmittingUnknown)
        return false;

      resultingString += "unknown";
      return true;
    }

    auto nameFromDecl = [&](Decl *d) -> StringRef {
      if (d) {
        if (auto accessor = dyn_cast<AccessorDecl>(d)) {
          return accessor->getStorage()->getBaseName().userFacingName();
        }
        if (auto vd = dyn_cast<ValueDecl>(d)) {
          return vd->getBaseName().userFacingName();
        }
      }

      return "<unknown decl>";
    };

    // Walk backwards, constructing our string.
    while (true) {
      auto next = variableNamePath.pop_back_val();

      if (auto *inst = next.dyn_cast<SILInstruction *>()) {
        if (auto i = DebugVarCarryingInst(inst)) {
          resultingString += i.getName();
        } else if (auto i = VarDeclCarryingInst(inst)) {
          resultingString += i.getName();
        } else if (auto f = dyn_cast<FunctionRefBaseInst>(inst)) {
          if (auto dc = f->getInitiallyReferencedFunction()->getDeclContext()) {
            resultingString += nameFromDecl(dc->getAsDecl());
          } else {
            resultingString += "<unknown decl>";
          }
        } else if (auto m = dyn_cast<MethodInst>(inst)) {
          resultingString += nameFromDecl(m->getMember().getDecl());
        } else {
          resultingString += "<unknown decl>";
        }
      } else {
        auto value = next.get<SILValue>();
        if (auto *fArg = dyn_cast<SILFunctionArgument>(value))
          resultingString += fArg->getDecl()->getBaseName().userFacingName();
      }

      if (variableNamePath.empty())
        return true;

      resultingString += '.';
    }

    return true;
  }
};

} // namespace swift

#endif
