//===--- Lazy.h -----------------------------------------------------------===//
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

#ifndef SWIFT_BASIC_LAZY_H
#define SWIFT_BASIC_LAZY_H

#include <type_traits>

namespace swift {

template <class T>
class Lazy {
  typename std::aligned_storage<sizeof(T), alignof(T)>::type Value;
  unsigned IsInitialized : 1;

public:
  Lazy() : Value(), IsInitialized(false) {}
  ~Lazy() {
    if (IsInitialized) {
      getValue().~T();
    }
  }

  operator T &() const {
    if (!IsInitialized) {
      IsInitialized = true;
      return *(new (Value.data) T());
    }
    return Value;
  }
};

} // namespace swift

#endif
