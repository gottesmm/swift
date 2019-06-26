
#ifndef SWIFTC_SWIFTBRIDGE_SILOPTIMIZERBRIDGEDTYPES_H
#define SWIFTC_SWIFTBRIDGE_SILOPTIMIZERBRIDGEDTYPES_H

#include "swift-c/SwiftBridge/SILBridgedTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TYPE(NAME)                                                             \
  typedef struct swiftbridge_silopt_##NAME {                                   \
    void *cxxImpl;                                                             \
  } swiftbridge_silopt_##NAME;

#define METHOD0(CXX_TYPE_NAME, CXX_METHOD_NAME, CXX_METHOD_RETURN,             \
                SWIFT_METHOD_RETURN)                                           \
  SWIFT_METHOD_RETURN swiftbridge_silopt_##CXX_TYPE_NAME##_##CXX_METHOD_NAME(  \
      swiftbridge_silopt_##CXX_TYPE_NAME instance);

#include "swift-c/SwiftBridge/SILOptimizerBridgedTypes.def"

#ifdef __cplusplus
}
#endif

#endif
