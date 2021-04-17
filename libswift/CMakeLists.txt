################################################################################
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for Swift project authors
################################################################################

if (NOT SWIFT_TOOLS_ENABLE_LIBSWIFT)
  # A dummy libswift if libswift is disabled
  add_swift_host_library(libswiftStub STATIC DisabledLibswiftStub.c)
  add_library(libswift ALIAS libswiftStub)
else()
  #project(libswift LANGUAGES C CXX Swift)
   
  # Add path for custom CMake modules.
  list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
  
  include(AddLibSwift)

  add_swift_host_library(LibSwift STATIC DisabledLibswiftStub.c)

  set(LIBSWIFT_MODULES "SIL" "Optimizer")
  foreach (module ${LIBSWIFT_MODULES})
    file(GLOB_RECURSE module_sources "${CMAKE_CURRENT_SOURCE_DIR}/Sources/${module}/*.swift")
    set(module_name "LibSwift${module}")
    add_swift_host_library(${module_name} STATIC ${module_sources})
    target_link_libraries(libswift STATIC ${module_name})
  endforeach()
endif()
