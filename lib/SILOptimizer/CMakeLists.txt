
set(SILOPTIMIZER_SOURCES)

function(_list_transform newvar)
  set(sources ${ARGN})
  set(dir ${CMAKE_CURRENT_SOURCE_DIR})
  set(tmp)
  foreach (s ${sources})
    list(APPEND tmp "${dir}/${s}")
  endforeach()
  set(${newvar} "${tmp}" PARENT_SCOPE)
endfunction()

macro(silopt_register_sources)
  precondition(new_transformed_sources
    NEGATE
    MESSAGE "Expected this to be empty since we clear after each run")
  _list_transform(new_transformed_sources ${ARGN})
  list_union("${SILOPTIMIZER_SOURCES}" "${new_transformed_sources}" out)
  set(SILOPTIMIZER_SOURCES "${out}" PARENT_SCOPE)
  set(new_transformed_sources)
endmacro()

set(SILOPTIMIZER_SWIFT_SOURCES)

macro(silopt_register_swift_sources)
  precondition(new_transformed_swift_sources
    NEGATE
    MESSAGE "Expected this to be empty since we clear after each run")
  _list_transform(new_transformed_swift_sources ${ARGN})
  list_union("${SILOPTIMIZER_SWIFT_SOURCES}" "${new_transformed_swift_sources}" out)
  set(SILOPTIMIZER_SWIFT_SOURCES "${out}" PARENT_SCOPE)
  set(new_transformed_swift_sources)
endmacro()

add_subdirectory(ARC)
add_subdirectory(Analysis)
add_subdirectory(FunctionSignatureTransforms)
add_subdirectory(IPO)
add_subdirectory(LoopTransforms)
add_subdirectory(Mandatory)
add_subdirectory(PassManager)
add_subdirectory(SILCombiner)
add_subdirectory(Transforms)
add_subdirectory(UtilityPasses)
add_subdirectory(Utils)

add_swift_host_library(swiftSILOptimizer STATIC
  ${SILOPTIMIZER_SOURCES})
target_link_libraries(swiftSILOptimizer PRIVATE
  swiftSIL
  swiftSILOptimizerSwiftBridge)

add_swift_host_library(swiftSILOptimizerSwift STATIC
  REQUIRES_SWIFTC
  ${SILOPTIMIZER_SWIFT_SOURCES})
target_compile_options(swiftSILOptimizerSwift PRIVATE "-import-objc-header" "${SWIFT_MAIN_INCLUDE_DIR}/swift-c/SwiftBridge/SwiftBridge.h")
target_compile_options(swiftSILOptimizerSwift PRIVATE "-I" "${SWIFT_MAIN_INCLUDE_DIR}/swift-c/SwiftBridge")
target_link_libraries(swiftSILOptimizerSwift PRIVATE swiftSILOptimizerSwiftBridge)
get_filename_component(TOOLCHAIN_BIN_DIR "${CMAKE_Swift_COMPILER}" DIRECTORY)
get_filename_component(TOOLCHAIN_SWIFT_LIB_DIR "${TOOLCHAIN_BIN_DIR}/../lib/swift/macosx" ABSOLUTE)
target_link_options(swiftSILOptimizerSwift PUBLIC "-L" "${TOOLCHAIN_SWIFT_LIB_DIR}")

target_link_libraries(swiftSILOptimizer PRIVATE swiftSILOptimizerSwift)
