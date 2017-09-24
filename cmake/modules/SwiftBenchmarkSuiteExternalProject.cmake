
include(CMakeParseArguments)
include(SwiftUtils)

function (add_benchmark_exec name)
  add_executable(${name} IMPORTED)
  set_target_properties(${name}
    PROPERTIES
    IMPORTED_LOCATION ${objdir}/bin/${name})
endfunction()

include(LLVMExternalProjectUtils)

function (add_benchmark_suite)
  cmake_parse_arguments(BENCH
    "EMIT_SIB"
    "SWIFT_EXEC;SWIFT_LIBRARY_PATH"
    "OPTIMIZATION_LEVELS" ${ARGN})
  
  precondition(SWIFT_EXEC)
  
  set(name swift-benchmark)
  set(src_dir ${SWIFT_SOURCE_DIR}/benchmark)
  set(bin_dir ${SWIFT_BINARY_DIR}/benchmark/binary)
  set(stamp_dir ${SWIFT_BINARY_DIR}/benchmark/stamps)
  set(prefix_dir ${SWIFT_BINARY_DIR}/benchmark/prefix)
  
  set(extra_targets Benchmark_O Benchmark_Onone Benchmark_Ounchecked swift-benchmark-macosx-x86_64)

  llvm_ExternalProject_add(swift-bench ${src_dir}
    SOURCE_DIR ${src_dir}
    EXCLUDE_FROM_ALL
    DEPENDS swift swift-stdlib-macosx
    EXTRA_TARGETS ${extra_targets}
    CMAKE_ARGS
    -DSWIFT_EXEC=${SWIFT_BINARY_DIR}/bin/swiftc
    -DCMAKE_C_COMPILER=${PATH_TO_CLANG_BUILD}/bin/clang
    -DCMAKE_CXX_COMPILER=${PATH_TO_CLANG_BUILD}/bin/clang++)
endfunction()
