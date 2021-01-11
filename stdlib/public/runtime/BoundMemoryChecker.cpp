//===--- BoundMemoryChecker.cpp -------------------------------------------===//
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

#include "../SwiftShims/_BoundMemoryChecker.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Lazy.h"
#include "swift/Basic/Range.h"
#include "swift/Runtime/Mutex.h"
#include "llvm/ADT/IntervalMap.h"
#include <atomic>

using namespace swift;

static StaticReadWriteLock BindMemoryStateLock;

/// An interval map we use to map intervals of the address space to specific
/// type ptrs.
namespace {
using IntervalMap = llvm::IntervalMap<uintptr_t, void *, 1>;
} // anonymous namespace
static Lazy<IntervalMap> BindMemoryState;

static std::tuple<IntervalMap::iterator, uintptr_t>
findFirstInterval(IntervalMap &map, uintptr_t start, uintptr_t end) {
  unsigned next = start;
  while (next != end) {
    auto value = map.find(next);
    if (value.valid())
      return {value, next};
    ++next;
  }

  return {map.end(), end};
}

void swift::swift_bindMemory(void *inputPtr, __swift_size_t bytes, void *type) {
  auto beginPtr = uintptr_t(inputPtr);
  auto endPtr = beginPtr + bytes;
  IntervalMap::iterator next;
  uintptr_t intervalAddr;

  StaticScopedWriteLock guard(BindMemoryStateLock);

  // First see if we have any value at ptr at all.
  auto &globalState = *BindMemoryState;
  std::tie(next, intervalAddr) = findFirstInterval(globalState, beginPtr, endPtr);

  // Didn't find anything, so we can just insert.
  if (next == globalState.end()) {
    return BindMemoryState->insert(beginPtr, endPtr, type);
  }

  // Ok, we do have some sort of overlap. First see if the initial overlap is
  // our type.
  if (next.value() != type) {
    // If it is not our type and the interval starts before our interval,
    // shorten the node without coalescing to no longer include our interval and
    // then go to the next interval if we have one.
    if (next.start() < intervalAddr) {
      auto tmp = next;
      ++tmp;
      next.setStopUnchecked(intervalAddr);
      next = tmp;
    } else {
      // Otherwise, we have a partial overlap. First set [beginPtr,intervalAddr)
    }
  } else {
    // Ok, we do have an overlap and the values line up, so go to the next
    // interval.
    ++next;
  }

  // Ok, now we know that 
}

bool swift::swift_isMemoryBoundToType(void *accessPtr, void *type) {
  StaticScopedReadLock guard(BindMemoryStateLock);
  return BindMemoryState->lookup(uintptr_t(accessPtr)) == type;
}
