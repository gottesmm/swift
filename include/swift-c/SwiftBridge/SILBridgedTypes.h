
#ifndef SWIFTC_SWIFTBRIDGE_SILBRIDGETYPES_H
#define SWIFTC_SWIFTBRIDGE_SILBRIDGETYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#define TYPE(NAME)                                                             \
  typedef struct swiftbridge_sil_##NAME {                                      \
    void *cxxImpl;                                                             \
  } swiftbridge_sil_##NAME;

#define METHOD0(CXX_TYPE_NAME, CXX_METHOD_NAME, CXX_METHOD_RETURN,             \
                SWIFT_METHOD_RETURN)                                           \
  SWIFT_METHOD_RETURN swiftbridge_sil_##CXX_TYPE_NAME##_##CXX_METHOD_NAME(     \
      swiftbridge_sil_##CXX_TYPE_NAME instance);

#include "swift-c/SwiftBridge/SILBridgedTypes.def"

#ifdef __cplusplus
}
#endif

#endif
