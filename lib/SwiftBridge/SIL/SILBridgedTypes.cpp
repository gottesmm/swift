//===--- SIL.cpp ----------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift-c/SwiftBridge/SILBridgedTypes.h"

// DO NOT INCLUDE CXX FILES ABOVE THIS!
#define SWIFTBRIDGE_MODULENAME sil
#include "../Helpers.h"
#include "swift/SIL/SILModule.h"

using namespace swift;

// This is where we define our thunks. Our thunks can use c++ in their
// implementation.
#define METHOD0(CXX_TYPE_NAME, CXX_METHOD_NAME, CXX_METHOD_RETURN,             \
                SWIFT_METHOD_RETURN)                                           \
  SWIFT_METHOD_RETURN swiftbridge_sil_##CXX_TYPE_NAME##_##CXX_METHOD_NAME(     \
      swiftbridge_sil_##CXX_TYPE_NAME instance) {                              \
    std::function<CXX_METHOD_RETURN(CXX_TYPE_NAME *, CXXArgs<>)> f =           \
        [&](CXX_TYPE_NAME *self, CXXArgs<> emptyTup) -> CXX_METHOD_RETURN {    \
      return self->CXX_METHOD_NAME();                                          \
    };                                                                         \
                                                                               \
    return Helper<CXX_TYPE_NAME, swiftbridge_sil_##CXX_TYPE_NAME,              \
                  CXX_METHOD_RETURN, SWIFT_METHOD_RETURN, CXXArgs<>,           \
                  SwiftArgs<>>()                                               \
        .invoke(instance, f);                                                  \
  }
#include "swift-c/SwiftBridge/SILBridgedTypes.def"
