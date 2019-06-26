
#ifndef SWIFT_SWIFTBRIDGE_HELPERS_H
#define SWIFT_SWIFTBRIDGE_HELPERS_H

#include "swift/Basic/STLExtras.h"
#include <tuple>

namespace swift {

template <typename... Args> struct CXXArgs {
  using types = std::tuple<Args...>;
};

template <typename... Args> struct SwiftArgs {
  using types = std::tuple<Args...>;
};

template <typename CXXSelfType, typename SwiftSelfType, typename CXXReturnValue,
          typename SwiftReturnValue, typename CXXArgs, typename SwiftArgs>
struct Helper {
  SwiftReturnValue
  invoke(SwiftSelfType self, typename SwiftArgs::type swiftArgs,
         std::function<CXXReturnValue *(CXXSelfType *, typename CXXArgs::types)>
             typeErasedCaller) {
    auto *cxxSelf = reinterpret_cast<CXXSelfType *>(self.cxxImpl);
    auto args = std::make_tuple(cxxSelf, swiftArgs);
    SwiftReturnValue result;
    result.cxxImpl = (void *)llvm::apply_tuple(typeErasedCaller, args);
    return result;
  }
};

template <typename CXXSelfType, typename SwiftSelfType, typename CXXReturnValue,
          typename SwiftReturnValue>
struct Helper<CXXSelfType, SwiftSelfType, CXXReturnValue, SwiftReturnValue,
              CXXArgs<>, SwiftArgs<>> {
  SwiftReturnValue
  invoke(SwiftSelfType self, SwiftArgs<>::types swiftArgs,
         std::function<CXXReturnValue *(CXXSelfType *, CXXArgs<>)>
             typeErasedCaller) {
    auto *cxxSelf = reinterpret_cast<CXXSelfType *>(self.cxxImpl);
    SwiftReturnValue result;
    result.cxxImpl = (void *)typeErasedCaller(cxxSelf, std::tuple<>());
    return result;
  }
};

template <typename CXXSelfType, typename SwiftSelfType>
struct Helper<CXXSelfType, SwiftSelfType, void, void, CXXArgs<>, SwiftArgs<>> {
  void invoke(SwiftSelfType self,
              std::function<void(CXXSelfType *, CXXArgs<>)> typeErasedCaller) {
    auto *cxxSelf = reinterpret_cast<CXXSelfType *>(self.cxxImpl);
    return typeErasedCaller(cxxSelf, CXXArgs<>());
  }
};

} // namespace swift

#ifdef SWIFT_TYPENAME
#error "SWIFT_TYPENAME already defined?!"
#endif

#ifndef SWIFTBRIDGE_MODULENAME
#error "Must define the module before including this!"
#endif

#define SWIFT_TYPENAME(NAME) swiftbridge_##SWIFTBRIDGE_MODULENAME##_##NAME

#endif
