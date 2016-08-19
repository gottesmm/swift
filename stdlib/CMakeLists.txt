
# TODO(compnerd) once we have a newer CMake we should be able to use the new
# `Swift` LANGUAGE support in CMake to simplify the build.  For now, just add
# the project so that it is possible to start working on extracting the Swift
# standard library from the Swift compiler build.
project(swift-stdlib LANGUAGES C CXX)

# Add path for custom CMake modules.
list(APPEND CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

# Create convenience targets for the Swift standard library.
set(SWIFT_STDLIB_BUILT_STANDALONE FALSE)
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  set(SWIFT_STDLIB_BUILT_STANDALONE TRUE)
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/modules")
endif()

include(AddSwiftStdlib)

# If we are not building standalone, perform the in tree configure and return.
if (NOT SWIFT_STDLIB_BUILT_STANDALONE)
  configure_intree_swift_stdlib()
  return()
endif()

cmake_minimum_required(VERSION 3.4.3)

# TODO: Fix RPATH usage to be CMP0068 compliant
# Disable Policy CMP0068 for CMake 3.9
# rdar://37725888
if(POLICY CMP0068)
  cmake_policy(SET CMP0068 OLD)
endif()

# Add path for custom CMake modules.
list(APPEND CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/modules")

# Make a job pool for things that can't yet be distributed
cmake_host_system_information(
  RESULT localhost_logical_cores QUERY NUMBER_OF_LOGICAL_CORES)
set_property(GLOBAL PROPERTY JOB_POOLS local_jobs=${localhost_logical_cores})
# Put linking in that category
set_property(GLOBAL PROPERTY JOB_POOL_LINK local_jobs)

ENABLE_LANGUAGE(C)

# First include general CMake utilities.
include(SwiftUtils)
include(CheckSymbolExists)

precondition(SWIFT_STDLIB_GLOBAL_CONFIG_CACHE_PATH MESSAGE "Variable cache not passed down?!")

message(STATUS "Global Cache Path: ${SWIFT_STDLIB_GLOBAL_CONFIG_CACHE_PATH}")

include(${SWIFT_STDLIB_GLOBAL_CONFIG_CACHE_PATH})

# Make sure that we reconfigure when the Global Cache Path changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SWIFT_STDLIB_GLOBAL_CONFIG_CACHE_PATH})

if (NOT SWIFT_BUILD_STDLIB AND NOT SWIFT_BUILD_SDK_OVERLAY)
  message(STATUS "Not building Swift standard library, SDK overlays, and runtime")
  message(STATUS "")
  return()
endif()

include(SwiftSharedCMakeConfig)

# We are always cross compiling
swift_common_standalone_build_config(SWIFT TRUE)

#
# Assume a new enough ar to generate the index at construction time. This avoids
# having to invoke ranlib as a secondary command.
#

set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> crs <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> qs <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "")

set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> crs <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qs <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "")

#
# Include CMake modules
#

include(CheckCXXSourceRuns)
include(CMakeParseArguments)
include(CMakePushCheckState)

# Look for either a program in execute_process()'s path or for a hardcoded path.
# Find a program's version and set it in the parent scope.
# Replace newlines with spaces so it prints on one line.
function(find_version cmd flag find_in_path)
  if(find_in_path)
    message(STATUS "Finding installed version for: ${cmd}")
  else()
    message(STATUS "Finding version for: ${cmd}")
  endif()
  execute_process(
    COMMAND ${cmd} ${flag}
    OUTPUT_VARIABLE out
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT out)
    if(find_in_path)
      message(STATUS "tried to find version for ${cmd}, but ${cmd} not found in path, continuing")
    else()
      message(FATAL_ERROR "tried to find version for ${cmd}, but ${cmd} not found")
    endif()
  else()
    string(REPLACE "\n" " " out2 ${out})
    message(STATUS "Found version: ${out2}")
  endif()
  message(STATUS "")
endfunction()

# Print out path and version of any installed commands.
# We migth be using the wrong version of a command, so record them all.
function(print_versions)
  find_version("${CMAKE_COMMAND}" "--version" TRUE)

  message(STATUS "Finding version for: ${CMAKE_COMMAND}")
  message(STATUS "Found version: ${CMAKE_VERSION}")
  message(STATUS "")

  get_filename_component(CMAKE_MAKE_PROGRAM_BN "${CMAKE_MAKE_PROGRAM}" NAME_WE)
  if(${CMAKE_MAKE_PROGRAM_BN} STREQUAL "ninja" OR
     ${CMAKE_MAKE_PROGRAM_BN} STREQUAL "make")
    find_version(${CMAKE_MAKE_PROGRAM_BN} "--version" TRUE)
    find_version(${CMAKE_MAKE_PROGRAM} "--version" FALSE)
  endif()

  if(${SWIFT_PATH_TO_CMARK_BUILD})
    find_version("cmark" "--version" TRUE)
    find_version("${SWIFT_PATH_TO_CMARK_BUILD}/src/cmark" "--version" FALSE)
  endif()

  message(STATUS "Finding version for: ${CMAKE_C_COMPILER}")
  message(STATUS "Found version: ${CMAKE_C_COMPILER_VERSION}")
  message(STATUS "")

  message(STATUS "Finding version for: ${CMAKE_CXX_COMPILER}")
  message(STATUS "Found version: ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "")
endfunction()
print_versions()

include(SwiftComponents)
include(SwiftHandleGybSources)
include(SwiftSetIfArchBitness)
include(SwiftSource)
include(AddSwift)
#include(SwiftConfigureSDK)
#include(SwiftComponents)
include(SwiftList)

# Configure swift include, install, build components.
#swift_configure_components()

if(NOT EXISTS "${CLANG_MAIN_INCLUDE_DIR}/clang/AST/Decl.h")
  message(FATAL_ERROR "Clang is missing from llvm/tools subdirectory.")
endif()

# This setting causes all CMakeLists.txt to automatically have
# ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CURRENT_SOURCE_DIR} as an
# include_directories path. This is done for developer
# convenience. Additionally, LLVM/Clang build with this option enabled, so we
# should match them unless it is removed from LLVM/Clang as well.
#
# *NOTE* Even though these directories are added to the include path for a
# specific CMakeLists.txt, these include paths are not propagated down to
# subdirectories.
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# We'll need this once we have generated headers
include_directories(BEFORE
  ${SWIFT_MAIN_INCLUDE_DIR}
  ${SWIFT_INCLUDE_DIR}
  )

# Xcode: use libc++ and c++11 using proper build settings.
if(XCODE)
  swift_common_xcode_cxx_config()
endif()

#
# Enable additional warnings.
#
swift_common_cxx_warnings()

#
# SDKs should have been configured in the parent CMakeLists.txt.
#

precondition(unknown_sdks NEGATE MESSAGE "Unknown SDKs: ${unknown_sdks}")
precondition(SWIFT_CONFIGURED_SDKS MESSAGE "No SDKs selected.")
precondition(SWIFT_HOST_VARIANT_SDK MESSAGE "No SDK for host tools.")
precondition(SWIFT_HOST_VARIANT_ARCH MESSAGE "No arch for host tools")

if(SWIFT_PARALLEL_LINK_JOBS)
  if(NOT CMAKE_MAKE_PROGRAM MATCHES "ninja")
    message(WARNING "Job pooling is only available with Ninja generators.")
  else()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS swift_link_job_pool=${SWIFT_PARALLEL_LINK_JOBS})
    set(CMAKE_JOB_POOL_LINK swift_link_job_pool)
  endif()
endif()

# Set the CMAKE_OSX_* variables in a way that minimizes conflicts.
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin" AND NOT CMAKE_CROSSCOMPILING)
  set(CMAKE_OSX_SYSROOT "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_PATH}")
  set(CMAKE_OSX_ARCHITECTURES "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "")
endif()

message(STATUS "Building Swift standard library and overlays for SDKs: ${SWIFT_SDKS}")
message(STATUS "  Build type:       ${SWIFT_STDLIB_BUILD_TYPE}")
message(STATUS "  Assertions:       ${SWIFT_STDLIB_ASSERTIONS}")
message(STATUS "  +0 Normal Args:   ${SWIFT_ENABLE_GUARANTEED_NORMAL_ARGUMENTS}")
message(STATUS "")

message(STATUS "Building Swift runtime with:")
message(STATUS "  Leak Detection Checker Entrypoints: ${SWIFT_RUNTIME_ENABLE_LEAK_CHECKER}")
message(STATUS "  +0 Normal Args:                     ${SWIFT_ENABLE_GUARANTEED_NORMAL_ARGUMENTS}")
message(STATUS "")

#
# Find required dependencies.
#

function(swift_icu_variables_set sdk arch result)
  string(TOUPPER "${sdk}" sdk)

  set(icu_var_ICU_UC_INCLUDE ${SWIFT_${sdk}_${arch}_ICU_UC_INCLUDE})
  set(icu_var_ICU_UC ${SWIFT_${sdk}_${arch}_ICU_UC})
  set(icu_var_ICU_I18N_INCLUDE ${SWIFT_${sdk}_${arch}_ICU_I18N_INCLUDE})
  set(icu_var_ICU_I18N ${SWIFT_${sdk}_${arch}_ICU_I18N})

  if(icu_var_ICU_UC_INCLUDE AND icu_var_ICU_UC AND
     icu_var_ICU_I18N_INCLUDE AND icu_var_ICU_I18N)
    set(${result} TRUE PARENT_SCOPE)
  else()
    set(${result} FALSE PARENT_SCOPE)
  endif()
endfunction()

# ICU is provided through CoreFoundation on Darwin.  On other hosts, if the ICU
# unicode and i18n include and library paths are not defined, perform a standard
# package lookup.  Otherwise, rely on the paths specified by the user.  These
# need to be defined when cross-compiling.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  if(SWIFT_BUILD_STDLIB OR SWIFT_BUILD_SDK_OVERLAY)
    swift_icu_variables_set("${SWIFT_PRIMARY_VARIANT_SDK}"
                            "${SWIFT_PRIMARY_VARIANT_ARCH}"
                            ICU_CONFIGURED)
    if("${SWIFT_PATH_TO_LIBICU_BUILD}" STREQUAL "" AND NOT ${ICU_CONFIGURED})
      find_package(ICU REQUIRED COMPONENTS uc i18n)
    endif()
  endif()
endif()

#
# Find optional dependencies.
#

# You need libedit linked in order to check if you have el_wgets.
cmake_push_check_state()
list(APPEND CMAKE_REQUIRED_LIBRARIES "edit")
check_symbol_exists(el_wgets "histedit.h" HAVE_EL_WGETS)
if(HAVE_EL_WGETS)
  set(HAVE_UNICODE_LIBEDIT 1)
endif()
cmake_pop_check_state()

check_symbol_exists(wait4 "sys/wait.h" HAVE_WAIT4)

check_symbol_exists(proc_pid_rusage "libproc.h" HAVE_PROC_PID_RUSAGE)
if(HAVE_PROC_PID_RUSAGE)
    list(APPEND CMAKE_REQUIRED_LIBRARIES proc)
endif()

if (LLVM_ENABLE_DOXYGEN)
  message(STATUS "Doxygen: enabled")
endif()

#
# The main CMakeLists.txt file should have set up global CMake variables for API
# notes.
#
if(NOT DEFINED SWIFT_API_NOTES_INPUTS)
  message(FATAL_ERROR "API notes are not available in ${SWIFT_API_NOTES_PATH}")
endif()

# NOTE(compnerd) this will pass the *build* configuration to the *host*
# libraries.  Explicitly indicate to CMake that it should **NOT** track the
# implicit language runtimes.  This can go away once we migrate to an external
# project with its own configure with the CMAKE_SYSTEM_NAME set rather than
# using the custom cross-compilation solution
set(CMAKE_C_IMPLICIT_LINK_LIBRARIES "")
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")

set(CMAKE_C_IMPLICIT_LINK_DIRECTORIES "")
set(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "")

if(SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER)
  if(NOT "${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    message(FATAL_ERROR "Building the swift runtime is not supported with ${CMAKE_C_COMPILER_ID}. Use the just-built clang instead.")
  else()
    message(WARNING "Building the swift runtime using the host compiler, and not the just-built clang.")
  endif()
else()
  # If we use Clang-cl or MSVC, CMake provides default compiler and linker flags that are incompatible
  # with the frontend of Clang or Clang++.
  if(SWIFT_COMPILER_IS_MSVC_LIKE)
    set(CMAKE_CXX_COMPILER "${SWIFT_NATIVE_LLVM_TOOLS_PATH}/clang-cl")
    set(CMAKE_C_COMPILER "${SWIFT_NATIVE_LLVM_TOOLS_PATH}/clang-cl")
  else()
    set(CMAKE_CXX_COMPILER "${SWIFT_NATIVE_LLVM_TOOLS_PATH}/clang++")
    set(CMAKE_C_COMPILER "${SWIFT_NATIVE_LLVM_TOOLS_PATH}/clang")
  endif()

  if(CMAKE_C_COMPILER_LAUNCHER MATCHES ".*distcc")
    set(CMAKE_C_COMPILER_LAUNCHER "")
  endif()
  if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES ".*distcc")
    set(CMAKE_CXX_COMPILER_LAUNCHER "")
  endif()

  # The sanitizers require using the same version of the compiler for
  # everything and there are various places where we link runtime code with
  # code built by the host compiler. Disable sanitizers for the runtime for
  # now.
  add_compile_options(-fno-sanitize=all)
endif()

# Do not enforce checks for LLVM's ABI-breaking build settings.
# The Swift runtime uses some header-only code from LLVM's ADT classes,
# but we do not want to link libSupport into the runtime. These checks rely
# on the presence of symbols in libSupport to identify how the code was
# built and cause link failures for mismatches. Without linking that library,
# we get link failures regardless, so instead, this just disables the checks.
if(CMAKE_VERSION VERSION_LESS 3.12)
  append("-DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1" CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
else()
  add_compile_definitions(LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1)
endif()

set(SWIFT_STDLIB_LIBRARY_BUILD_TYPES)
if(SWIFT_BUILD_DYNAMIC_STDLIB)
  list(APPEND SWIFT_STDLIB_LIBRARY_BUILD_TYPES SHARED)
endif()
if(SWIFT_BUILD_STATIC_STDLIB)
  list_intersect("${SWIFT_APPLE_PLATFORMS}" "${SWIFT_SDKS}" building_darwin_sdks)
  if(building_darwin_sdks)
    message(SEND_ERROR "cannot build static standard library for Darwin SDKs")
  else()
    list(APPEND SWIFT_STDLIB_LIBRARY_BUILD_TYPES STATIC)
  endif()
endif()

function(swift_create_stdlib_targets name variant define_all_alias)
  if(NOT variant STREQUAL "")
    set(variant "-${variant}")
  endif()

  if(define_all_alias)
    add_custom_target(${name}${variant}-all)
    set_target_properties(${name}${variant}-all
      PROPERTIES
      FOLDER "Swift libraries/Aggregate")
  endif()

  foreach(sdk ${SWIFT_SDKS})
    add_custom_target(${name}-${SWIFT_SDK_${sdk}_LIB_SUBDIR}${variant})
    set_target_properties(${name}-${SWIFT_SDK_${sdk}_LIB_SUBDIR}${variant}
      PROPERTIES
      FOLDER "Swift libraries/Aggregate")

    foreach(arch ${SWIFT_SDK_${sdk}_ARCHITECTURES})
      set(target_variant -${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch})

      add_custom_target(${name}${target_variant}${variant})
      set_target_properties(${name}${target_variant}${variant}
        PROPERTIES
        FOLDER "Swift libraries/Aggregate")
      if(define_all_alias)
        add_dependencies(${name}${variant}-all
          ${name}${target_variant}${variant})
      endif()
      add_dependencies(${name}-${SWIFT_SDK_${sdk}_LIB_SUBDIR}${variant}
        ${name}${target_variant}${variant})
    endforeach()
  endforeach()

  if(NOT define_all_alias)
    set(ALL_keyword ALL)
  endif()
  add_custom_target(${name}${variant}
    ${ALL_keyword}
    DEPENDS
    ${name}${SWIFT_PRIMARY_VARIANT_SUFFIX}${variant})
  set_target_properties(${name}${variant}
    PROPERTIES
    FOLDER "Swift libraries/Aggregate")
endfunction()

swift_create_stdlib_targets("swift-stdlib" "" TRUE)
if(SWIFT_STDLIB_ENABLE_SIB_TARGETS)
  swift_create_stdlib_targets("swift-stdlib" "sib" TRUE)
  swift_create_stdlib_targets("swift-stdlib" "sibopt" TRUE)
  swift_create_stdlib_targets("swift-stdlib" "sibgen" TRUE)
endif()
swift_create_stdlib_targets("swift-test-stdlib" "" FALSE)

# FIXME: Include the toolchain directory before the public directory. Otherwise
# the clang resource directory symlink stops installing correctly.
add_subdirectory(toolchain)
add_subdirectory(public)
add_subdirectory(private)
add_subdirectory(tools)

# Process our cmake/modules after we have created everything so we can create our exports file.
add_subdirectory(cmake/modules)

