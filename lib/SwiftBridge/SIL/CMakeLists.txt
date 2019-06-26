
add_swift_host_library(swiftSILSwiftBridge STATIC
  SILBridgedTypes.cpp)

add_dependencies(swiftSILSwiftBridge intrinsics_gen clang-tablegen-targets)
