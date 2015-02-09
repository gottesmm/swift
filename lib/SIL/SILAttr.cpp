//===--- SILAttr.cpp ------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILAttr.h"
#include "swift/SIL/SILFunction.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                           SILAttrKind Functions
//===----------------------------------------------------------------------===//

SILAttrValueKind swift::getValueKindForAttrKind(SILAttrKind Kind) {
  switch (Kind) {
#define FUNCTION_ATTRIBUTE_STRING(Name) case SILAttrKind::Function_##Name:
#include "swift/SIL/SILAttr.def"
    return SILAttrValueKind::String;
#define FUNCTION_ATTRIBUTE_BOOLEAN(Name) case SILAttrKind::Function_##Name:
#include "swift/SIL/SILAttr.def"
    return SILAttrValueKind::Boolean;
#define FUNCTION_ATTRIBUTE_SINGLETON_ENUM(Name)                                \
  case SILAttrKind::Function_##Name:
#include "swift/SIL/SILAttr.def"
    return SILAttrValueKind::SingletonEnum;
#define FUNCTION_ATTRIBUTE_SET_ENUM(Name) case SILAttrKind::Function_##Name:
#include "swift/SIL/SILAttr.def"
    return SILAttrValueKind::SetEnum;
  }
}

bool swift::isSILAttrValueKindUnique(SILAttrValueKind Kind) {
  switch (Kind) {
  case SILAttrValueKind::String:
    return false;
  case SILAttrValueKind::Boolean:
    return true;
  case SILAttrValueKind::SetEnum:
    return false;
  case SILAttrValueKind::SingletonEnum:
    return true;
  }
}

bool swift::isSILAttrKindUnique(SILAttrKind Kind) {
  return isSILAttrValueKindUnique(getValueKindForAttrKind(Kind));
}

SILAttrValueKind
swift::getAttrValueKindForAttrEnumValue(SILAttrEnumValue Value) {
  switch (Value) {
#define FUNCTION_ATTRIBUTE_SINGLETON_ENUM_VALUE(Kind, Value)                   \
  case SILAttrEnumValue::Function_Singleton_##Kind##_##Value:
#include "swift/SIL/SILAttr.def"
    return SILAttrValueKind::SingletonEnum;
#define FUNCTION_ATTRIBUTE_SET_ENUM_VALUE(Kind, Value)                         \
  case SILAttrEnumValue::Function_Set_##Kind##_##Value:
#include "swift/SIL/SILAttr.def"
    return SILAttrValueKind::SetEnum;
  }
}
