//===--- _BoundMemoryChecker.h --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Define types and support functions for the Bound Memory Checker.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BOUNDMEMORYCHECKER_H
#define SWIFT_BOUNDMEMORYCHECKER_H

#include "SwiftStddef.h"

#ifdef __cplusplus
namespace swift {
extern "C" {
#endif

void swift_bindMemory(void *ptr, __swift_size_t bytes, void *type);
bool swift_isMemoryBoundToType(void *accessPtr, void *type);

#ifdef __cplusplus
} // extern "C"
} // namespace swift
#endif

#endif
