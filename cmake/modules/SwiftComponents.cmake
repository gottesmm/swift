include(SwiftList)

# Swift Components
# ----------------
#
# This file contains the cmake code for initialization and manipulation of
# "Swift Components". A "Swift Component" is a disjoint set of source files,
# binary products, and source products that are inputs or products of Swift's
# cmake build system. At a high level each component can be viewed approximately
# as an individual package in a Debian-style Linux package (i.e. a .deb file).
#
# *NOTE* In the following for brevity, a "Swift Component" will just be called a
# component.
#
# For every cmake invocation, the set of components are split into the following
# sets: defined, include, build, and install components. These sets form a
# lattice as follows:
#
#    install => build => include => defined
#
# We describe the characteristics of each set below:
#
# 1. A "defined" component is a component that is known to the build system. It
# has defined source inputs and binary/source outputs. But the build system is
# not required to generate targets, run any targets associated with the package
# while building, or install any binary/source outputs associated with the
# component.
#
# 2. A "include" component is a "defined" component that cmake will generate
# targets for. This means that it will not be built or installed by default
# (i.e. it is not apart of the "all" target), but one can from the relevant
# cmake build command invoke the generated targets directly and any dependencies
# on the component from other packages will cause the package to be built and or
# installed.
#
# 3. A "build" component is a "include" component that cmake will add to the
# "all" target. This means that cmake will add this component to the "all"
# target causing it to be built by default when one invokes a build tool. On the
# other hand, the component is not guaranteed to be built by default.
#
# 4. A "install" component is a "build" component that will have targets
# generated by default, will be built by default, and will be installed by
# default.
#
# Set of Defined Components
# -------------------------
#
# The set of "defined" swift components are as follows:
#
# * autolink-driver -- the Swift driver support tools
# * compiler -- the Swift compiler and (on supported platforms) the REPL.
# * clang-builtin-headers -- install a copy of Clang builtin headers under
#   'lib/swift/clang'.  This is useful when Swift compiler is installed in
#   isolation.
# * clang-resource-dir-symlink -- install a symlink to the Clang resource
#   directory (which contains builtin headers) under 'lib/swift/clang'.  This is
#   useful when Clang and Swift are installed side-by-side.
# * stdlib -- the Swift standard library.
# * stdlib-experimental -- the Swift standard library module for experimental
#   APIs.
# * sdk-overlay -- the Swift SDK overlay.
# * editor-integration -- scripts for Swift integration in IDEs other than
#   Xcode;
# * tools -- tools (other than the compiler) useful for developers writing
#   Swift code.
# * testsuite-tools -- extra tools required to run the Swift testsuite.
# * toolchain-dev-tools -- install development tools useful in a shared toolchain
# * dev -- headers and libraries required to use Swift compiler as a library.
set(_SWIFT_DEFINED_COMPONENTS
  "autolink-driver;compiler;clang-builtin-headers;clang-resource-dir-symlink;clang-builtin-headers-in-clang-resource-dir;stdlib;stdlib-experimental;sdk-overlay;editor-integration;tools;testsuite-tools;toolchain-dev-tools;dev;license;sourcekit-xpc-service;sourcekit-inproc;swift-remote-mirror;swift-remote-mirror-headers")

# Configure the "Swift Component" system.
#
# *NOTE* We generally prefer functions over macros, but in this case, we need a
# macro to insure that SWIFT_{INCLUDE,BUILD,INSTALL}_COMPONENTS are
# available/cached at the top CMake scope.
macro(swift_configure_components)
  # Define our sets of components. By default, all components are "install
  # components".
  set(SWIFT_INCLUDE_COMPONENTS "${_SWIFT_DEFINED_COMPONENTS}" CACHE STRING
    "A semicolon-separated list of components to generate cmake targets for. \
Must be disjoint from SWIFT_BUILD_COMPONENTS and SWIFT_INSTALL_COMPONENTS")
  set(SWIFT_BUILD_COMPONENTS "${_SWIFT_DEFINED_COMPONENTS}" CACHE STRING
    "A semicolon-separated list of components to build by default. Must be \
disjoint from SWIFT_INCLUDE_COMPONENTS and SWIFT_INSTALL_COMPONENTS")
  set(SWIFT_INSTALL_COMPONENTS "${_SWIFT_DEFINED_COMPONENTS}" CACHE STRING
    "A semicolon-separated list of components to install ${_SWIFT_DEFINED_COMPONENTS}")

  # Make sure each component set does not have duplicates. This is necessary
  # since these sets are represented in cmake as lists.
  precondition_list_is_set(SWIFT_INCLUDE_COMPONENTS)
  precondition_list_is_set(SWIFT_BUILD_COMPONENTS)
  precondition_list_is_set(SWIFT_INSTALL_COMPONENTS)

  # Then make sure that each one of our sets are disjoint.
  precondition_list_is_disjoint(
    SWIFT_INCLUDE_COMPONENTS
    SWIFT_BUILD_COMPONENTS
    SWIFT_INSTALL_COMPONENTS)

  foreach(component ${_SWIFT_DEFINED_COMPONENTS})
    string(TOUPPER "${component}" var_name_piece)
    string(REPLACE "-" "_" var_name_piece "${var_name_piece}")
    set(SWIFT_INSTALL_${var_name_piece} FALSE)
  endforeach()

  foreach(component ${SWIFT_INSTALL_COMPONENTS})
    list(FIND _SWIFT_DEFINED_COMPONENTS "${component}" index)
    if(${index} EQUAL -1)
      message(FATAL_ERROR "unknown install component: ${component}")
    endif()

    string(TOUPPER "${component}" var_name_piece)
    string(REPLACE "-" "_" var_name_piece "${var_name_piece}")
    set(SWIFT_INSTALL_${var_name_piece} TRUE)
  endforeach()
endmacro()

function(swift_is_installing_component component result_var_name)
  precondition(component MESSAGE "Component name is required")

  if("${component}" STREQUAL "never_install")
    set("${result_var_name}" FALSE PARENT_SCOPE)
  else()
    list(FIND _SWIFT_DEFINED_COMPONENTS "${component}" index)
    if(${index} EQUAL -1)
      message(FATAL_ERROR "unknown install component: ${component}")
    endif()

    string(TOUPPER "${component}" var_name_piece)
    string(REPLACE "-" "_" var_name_piece "${var_name_piece}")
    set("${result_var_name}" "${SWIFT_INSTALL_${var_name_piece}}" PARENT_SCOPE)
  endif()
endfunction()

# swift_install_in_component(<COMPONENT NAME>
#   <same parameters as install()>)
#
# Executes the specified installation actions if the named component is
# requested to be installed.
#
# This function accepts the same parameters as install().
function(swift_install_in_component component)
  precondition(component MESSAGE "Component name is required")

  swift_is_installing_component("${component}" is_installing)
  if(is_installing)
    install(${ARGN})
  endif()
endfunction()
