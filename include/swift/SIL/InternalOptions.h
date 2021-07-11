//===--- InternalOptions.h ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_IR_INTERNALOPTIONS_H
#define SWIFT_SIL_IR_INTERNALOPTIONS_H

#include "swift/AST/Types.h"

namespace swift {
namespace sil {

extern bool DestructureLargeTuples;
extern unsigned DestructureLargeTupleThreshold;

static inline bool shouldDestructureTuple(CanTupleType tupleTy) {
  //if (!DestructureLargeTuples)
  //  return true;
  return tupleTy.getElementTypes().size() < DestructureLargeTupleThreshold;
}

} // namespace sil
} // namespace swift

#endif

