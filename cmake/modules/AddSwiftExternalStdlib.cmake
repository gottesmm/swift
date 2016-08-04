
macro(add_external_stdlib)
  if(CMAKE_VERSION VERSION_GREATER 3.3.20150708)
    set(cmake_3_4_USES_TERMINAL_OPTIONS
      USES_TERMINAL_CONFIGURE 1
      USES_TERMINAL_BUILD 1
      USES_TERMINAL_INSTALL 1
      )
  endif()

  if(CMAKE_VERSION VERSION_GREATER 3.1.20141117)
    set(cmake_3_2_USES_TERMINAL USES_TERMINAL)
  endif()

  # Add swift-stdlib as an external project.
  set(SWIFT_STDLIB_PREFIX ${CMAKE_BINARY_DIR}/stdlib)

  set(STAMP_DIR ${CMAKE_CURRENT_BINARY_DIR}/swift-stdlib-stamps/)
  set(BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/swift-stdlib-bins/)

  add_custom_target(swift-stdlib-clear
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${STAMP_DIR}
    COMMENT "Clobberring swift-stdlib build and stamp directories"
    )

  # Find all variables that start with SWIFT_STDLIB and populate a variable with
  # them.
  get_cmake_property(variableNames VARIABLES)
  foreach(variableName ${variableNames})
    if(variableName MATCHES "^SWIFT_STDLIB")
      string(REPLACE ";" "\;" value "${${variableName}}")
      list(APPEND SWIFT_STDLIB_PASSTHROUGH_VARIABLES
        -D${variableName}=${value})
    endif()
  endforeach()

  ExternalProject_Add(swift-stdlib
    DEPENDS clang swiftc llvm-config
    PREFIX ${SWIFT_STDLIB_PREFIX}
    SOURCE_DIR ${SWIFT_STDLIB_SRC_ROOT}
    STAMP_DIR ${STAMP_DIR}
    BINARY_DIR ${BINARY_DIR}
    CMAKE_ARGS ${CLANG_SWIFT_STDLIB_CMAKE_ARGS}
               -DCMAKE_C_COMPILER=${LLVM_RUNTIME_OUTPUT_INTDIR}/clang
               -DCMAKE_CXX_COMPILER=${LLVM_RUNTIME_OUTPUT_INTDIR}/clang++
               -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
               -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
               -DLLVM_CONFIG_PATH=${LLVM_RUNTIME_OUTPUT_INTDIR}/llvm-config
               -DLLVM_LIT_ARGS=${LLVM_LIT_ARGS}
               -DSWIFT_STDLIB_OUTPUT_DIR=${LLVM_LIBRARY_OUTPUT_INTDIR}/swift/${SWIFT_VERSION}
               -DSWIFT_STDLIB_EXEC_OUTPUT_DIR=${LLVM_RUNTIME_OUTPUT_INTDIR}
               -DSWIFT_STDLIB_INSTALL_PATH:STRING=lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_VERSION}
               -DSWIFT_STDLIB_INCLUDE_TESTS=${SWIFT_INCLUDE_TESTS}
               -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
               ${SWIFT_STDLIB_PASSTHROUGH_VARIABLES}
    INSTALL_COMMAND ""
    STEP_TARGETS configure build
    ${cmake_3_4_USES_TERMINAL_OPTIONS}
    )

  get_ext_project_build_command(run_clean_swift_stdlib clean)
  ExternalProject_Add_Step(swift-stdlib clean
    COMMAND ${run_clean_swift_stdlib}
    COMMENT "Cleaning swift-stdlib..."
    DEPENDEES configure
    DEPENDERS build
    DEPENDS clang
    WORKING_DIRECTORY ${BINARY_DIR}
    )

  install(CODE "execute_process\(COMMAND \${CMAKE_COMMAND} -DCMAKE_INSTALL_PREFIX=\${CMAKE_INSTALL_PREFIX} -P ${BINARY_DIR}/cmake_install.cmake \)"
    COMPONENT swift-stdlib)

  add_custom_target(install-swift-stdlib
                    DEPENDS swift-stdlib
                    COMMAND "${CMAKE_COMMAND}"
                             -DCMAKE_INSTALL_COMPONENT=swift-stdlib
                             -P "${CMAKE_BINARY_DIR}/cmake_install.cmake"
                    ${cmake_3_2_USES_TERMINAL})

  # Add top-level targets that build specific swift-stdlib runtimes.
  set(SWIFT_STDLIB_RUNTIMES swift-stdlib-all)
  foreach(runtime ${SWIFT_STDLIB_RUNTIMES})
    get_ext_project_build_command(build_runtime_cmd ${runtime})
    add_custom_target(${runtime}
      COMMAND ${build_runtime_cmd}
      DEPENDS swift-stdlib-configure
      WORKING_DIRECTORY ${BINARY_DIR}
      VERBATIM ${cmake_3_2_USES_TERMINAL})
  endforeach()

  if(LLVM_INCLUDE_TESTS)
    # Add binaries that swift-stdlib tests depend on.
    set(SWIFT_STDLIB_TEST_DEPENDENCIES
      FileCheck count not llvm-nm llvm-objdump llvm-symbolizer)

    # Add top-level targets for various swift-stdlib test suites.
    set(SWIFT_STDLIB_TEST_SUITES check-swift-stdlib-all)
    foreach(test_suite ${SWIFT_STDLIB_TEST_SUITES})
      get_ext_project_build_command(run_test_suite ${test_suite})
      add_custom_target(${test_suite}
        COMMAND ${run_test_suite}
        DEPENDS swift-stdlib-build ${SWIFT_STDLIB_TEST_DEPENDENCIES}
        WORKING_DIRECTORY ${BINARY_DIR}
        VERBATIM ${cmake_3_2_USES_TERMINAL})
    endforeach()

    # Add special target to run all swift-stdlib test suites.
    get_ext_project_build_command(run_check_swift_stdlib check-all)
    add_custom_target(check-swift-stdlib
      COMMAND ${run_check_swift_stdlib}
      DEPENDS swift-stdlib-build ${SWIFT_STDLIB_TEST_DEPENDENCIES}
      WORKING_DIRECTORY ${BINARY_DIR}
      VERBATIM ${cmake_3_2_USES_TERMINAL})
    set_property(GLOBAL APPEND PROPERTY LLVM_LIT_DEPENDS check-swift-stdlib)
  endif()
endmacro()
