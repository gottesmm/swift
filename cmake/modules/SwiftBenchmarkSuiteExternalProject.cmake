
include(CMakeParseArguments)
include(SwiftUtils)

function (add_benchmark_exec name)
  add_executable(${name} IMPORTED)
  set_target_properties(${name}
    PROPERTIES
    IMPORTED_LOCATION ${objdir}/bin/${name})
endfunction()

function (add_benchmark_suite)
  cmake_parse_arguments(BENCH
    "EMIT_SIB"
    "SWIFT_EXEC;SWIFT_LIBRARY_PATH"
    "OPTIMIZATION_LEVELS" ${ARGN})
      
  precondition(SWIFT_EXEC)

  set(srcdir "${SWIFT_SOURCE_DIR}/benchmark")
  set(objdir "${SWIFT_BINARY_DIR}/benchmark")
  set(build_byproducts
    ${objdir}/bin/Benchmark_Onone
    ${objdir}/bin/Benchmark_O
    ${objdir}/bin/Benchmark_Ounchecked)

  include(ExternalProject)
  ExternalProject_Add(swift_bench
    SOURCE_DIR ${srcdir}
    BINARY_DIR ${objdir}
    CMAKE_ARGS
    -DCMAKE_C_COMPILER=${PATH_TO_CLANG_BUILD}/bin/clang
    -DCMAKE_CXX_COMPILER=${PATH_TO_CLANG_BUILD}/bin/clang++
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DSWIFT_EXEC="${SWIFT_BINARY_DIR}/bin/swiftc"
    -DSWIFT_OPTIMIZATION_LEVELS="${BENCH_OPTIMIZATION_LEVELS}"
    BUILD_BYPRODUCTS ${build_byproducts})

  #include_directories(AFTER
  #  ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}/src/BlocksRuntime)
  #link_directories(${SWIFT_PATH_TO_LIBDISPATCH_BUILD})

  #include_directories(AFTER ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE})

  add_benchmark_exec(Benchmark_Onone)
  add_benchmark_exec(Benchmark_O)
  add_benchmark_exec(Benchmark_Ounchecked)

  #add_library(swiftCore SHARED IMPORTED)
  #set_target_properties(swiftCore PROPERTIES
  #  IMPORTED_LOCATION ${SOURCEKIT_BINARY_DIR}/lib/swift/linux/libswiftCore.so)

  #set(SOURCEKIT_NEED_EXPLICIT_LIBDISPATCH TRUE)
endfunction()
