//===--- SILAttr.h --------------------------------------------------------===//
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
///
/// \file
/// This file defines the SILAttr class. The SILAttr class is a general uniqued
/// attribute class for SIL. It is based around metaprogramming in SILAttr.def to
/// minimize the amount of updating that is needed to add new attributes.
///
/// SIL Attributes have the form:
///
///    (AttributeKind, AttributeValueKind, Value).
///
/// This is so that all attributes can have a general attribute, e.g. semantics,
/// or effects and a specific attribute value, e.g. "stdlib_binary_only" or
/// "readonly". Thus an AttributeKind would be something like:
/// Function_Semantics, i.e. AttributeLocation ## _ ## AttributeName, an
/// attribute value kind would be one of Boolean, String, Enum. Value is then
/// something of that attribute value kind.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_ATTRIBUTE_H
#define SWIFT_SIL_ATTRIBUTE_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/NullablePtr.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/Support/Allocator.h"
#include <cstdint>

namespace swift {

class SILAttrCache;
class SILFunction;

//===----------------------------------------------------------------------===//
//                                  SILAttr
//===----------------------------------------------------------------------===//

/// The general kind of an attribute. Meant to represent the name of the
/// attribute, not the value of the attribute.
enum class SILAttrKind : uint8_t {
#define ATTRIBUTE(Location, Kind) Location##_##Kind,
#include "SILAttr.def"
};

/// The type of value that a SILAttr is storing.
enum class SILAttrValueKind : uint8_t {
  String,
  Boolean,
  Integer,
  Enum,
};

SILAttrValueKind getValueKindForAttrKind(SILAttrKind Kind);

/// You only need to give an EnumKind argument if the first argument is a
/// SILAttrValueKind::Enum. For other SILAttrValueKind it is ignored. If you do
/// not set it when SILAttrValueKind::Enum is set, you will hit an unreachable.
bool isSILAttrValueKindUnique(SILAttrValueKind Kind);

bool isSILAttrKindUnique(SILAttrKind Kind);

/// The values for all SILAttrValueKind::Enum.
enum class SILAttrEnumValue : uint8_t {
#define FUNCTION_ATTRIBUTE_ENUM_SET_VALUE(Kind, Value)                         \
  Function_Enum_##Kind##_##Value,
#define FUNCTION_ATTRIBUTE_ENUM_SET_VALUE_RANGE(Kind, Start, End)              \
  Function_Enum_##Kind##_Start = Function_Enum_##Kind##_##Start,               \
  Function_Enum_##Kind##_End = Function_Enum_##Kind##_##End,
#include "SILAttr.def"
};

SILAttrValueKind getAttrValueKindForAttrEnumValue(SILAttrEnumValue Value);

/// A location in the SIL code where an attribute can be stored.
enum class SILAttrLocation : uint8_t { Function, Parameter, Return };

SILAttrLocation getLocationForAttrKind(SILAttrKind Kind);

class SILAttr {
  friend class SILAttrCache;
  friend class SILFunction;

  SILAttrKind AttrKind;
  SILAttrValueKind ValueKind;

  union {
    StringRef StringValue;
    bool BooleanValue;
    unsigned IntegerValue;
    SILAttrEnumValue EnumValue;
  };

  SILAttr(SILAttrKind AttrKind, StringRef StringValue)
    : AttrKind(AttrKind), ValueKind(SILAttrValueKind::String),
      StringValue(StringValue) {
    assert((getValueKindForAttrKind(AttrKind) == ValueKind) &&
           "Invalid ValueKind for AttrKind!");
  }

  SILAttr(SILAttrKind AttrKind, bool BooleanValue)
    : AttrKind(AttrKind), ValueKind(SILAttrValueKind::Boolean),
      BooleanValue(BooleanValue) {
    assert((getValueKindForAttrKind(AttrKind) == ValueKind) &&
           "Invalid ValueKind for AttrKind!");
  }

  SILAttr(SILAttrKind AttrKind, unsigned IntegerValue)
      : AttrKind(AttrKind), ValueKind(SILAttrValueKind::Integer),
        IntegerValue(IntegerValue) {
    assert((getValueKindForAttrKind(AttrKind) == ValueKind) &&
           "Invalid ValueKind for AttrKind!");
  }

  SILAttr(SILAttrKind AttrKind, SILAttrEnumValue EnumValue)
      : AttrKind(AttrKind), ValueKind(SILAttrValueKind::Enum),
        EnumValue(EnumValue) {
    assert((getValueKindForAttrKind(AttrKind) == ValueKind) &&
           "Invalid ValueKind for AttrKind!");
  }

public:
  /// Do we allow for this SILAttrKind to have multiple values?
  bool isUnique() const;

  /// \returns the location where this Attribute may be placed.
  SILAttrLocation getLocation() const;

  /// Return the kind of this attribute. This is a type that is worked with via
  /// metaprogramming in SILAttr.def.
  SILAttrKind getAttrKind() const { return AttrKind; }

  /// Return the kind of value we are storing. This can be one of bool, string,
  /// or enum.
  SILAttrValueKind getValueKind() const { return ValueKind; }

  /// If this is an Attribute with a string value, return it. Asserts if this
  /// does not contain a string value.
  StringRef getStringValue() const {
    assert(ValueKind == SILAttrValueKind::String &&
           "Expected to have string value type");
    return StringValue;
  }

  /// If this is a SILAttr with an integer value, return it. Otherwise assert.
  bool getIntValue() const {
    assert(ValueKind == SILAttrValueKind::Integer &&
           "Expected to have boolean value type");
    return IntegerValue;
  }

  /// If this is a SILAttr with a boolean value, return it. Otherwise assert.
  bool getBooleanValue() const {
    assert(ValueKind == SILAttrValueKind::Boolean &&
           "Expected to have boolean value type");
    return BooleanValue;
  }

  /// If this is a SILAttr with an enum value, return it. Otherwise assert.
  SILAttrEnumValue getEnumValue() const {
    assert(ValueKind == SILAttrValueKind::Enum &&
           "Expected to have enum value type");
    return EnumValue;
  }

private:
  void setValue(bool NewValue) {
    assert(ValueKind == SILAttrValueKind::Boolean &&
           "Expected to have a boolean value type");
    BooleanValue = NewValue;
  }

  void setValue(unsigned NewValue) {
    assert(ValueKind == SILAttrValueKind::Integer &&
           "Expected to have a integer value type");
    IntegerValue = NewValue;
  }
};

} // end swift namespace

#endif
