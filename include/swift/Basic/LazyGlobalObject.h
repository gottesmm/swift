//===--- LazyGlobalObject.h -----------------------------------------------===//
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

#ifndef SWIFT_BASIC_LAZYGLOBALOBJECT_H
#define SWIFT_BASIC_LAZYGLOBALOBJECT_H

#include <memory>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#else
#include <mutex>
#endif
#include "swift/Basic/Malloc.h"
#include "swift/Basic/type_traits.h"

namespace swift {

#ifdef __APPLE__
using OnceToken_t = dispatch_once_t;
#define SWIFT_ONCE_F(TOKEN, FUNC, CONTEXT)                                     \
  ::dispatch_once_f(&TOKEN, CONTEXT, FUNC)
#elif defined(__CYGWIN__)
// _swift_once_f() is declared in Private.h.
// This prototype is copied instead including the header file.
void _swift_once_f(uintptr_t *predicate, void *context,
                   void (*function)(void *));
using OnceToken_t = unsigned long;
#define SWIFT_ONCE_F(TOKEN, FUNC, CONTEXT) _swift_once_f(&TOKEN, CONTEXT, FUNC)
#else
using OnceToken_t = std::once_flag;
#define SWIFT_ONCE_F(TOKEN, FUNC, CONTEXT)                                     \
  ::std::call_once(TOKEN, FUNC, CONTEXT)
#endif

/// A template for lazily-constructed, zero-initialized, leaked-on-exit
/// global objects.
template <class T> class LazyGlobalObject {
  typename std::aligned_storage<sizeof(T), alignof(T)>::type Value;

  OnceToken_t OnceToken;

  static void defaultInitCallback(void *ValueAddr) { ::new (ValueAddr) T(); }

public:
  using Type = T;

  T &get(void (*initCallback)(void *) = defaultInitCallback);

  /// Get the value, assuming it must have already been initialized by this
  /// point.
  T &unsafeGetAlreadyInitialized() { return *reinterpret_cast<T *>(&Value); }

  constexpr LazyGlobalObject() = default;

  T *operator->() { return &get(); }
  T &operator*() { return get(); }

private:
  LazyGlobalObject(const LazyGlobalObject &) = delete;
  LazyGlobalObject &operator=(const LazyGlobalObject &) = delete;
};

template <typename T>
inline T &LazyGlobalObject<T>::get(void (*initCallback)(void *)) {
  static_assert(std::is_literal_type<LazyGlobalObject<T>>::value,
                "LazyGlobalObject<T> must be a literal type");

  SWIFT_ONCE_F(OnceToken, initCallback, &Value);
  return unsafeGetAlreadyInitialized();
}

} // end namespace swift

#define SWIFT_LAZYGLOBALOBJECT_CONSTANT(INITIAL_VALUE)                         \
  ([] {                                                                        \
    using T = ::std::remove_reference<decltype(INITIAL_VALUE)>::type;          \
    static ::swift::LazyGlobalObject<T> TheLazyGlobalObject;                   \
    return TheLazyGlobalObject.get(                                            \
        [](void *ValueAddr) { ::new (ValueAddr) T{INITIAL_VALUE}; });          \
  }())

#endif // SWIFT_BASIC_LAZY_H
