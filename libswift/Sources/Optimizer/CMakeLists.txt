# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for Swift project authors

add_swift_host_library(LibSwiftOptimizer
  STATIC PURE_SWIFT Empty.swift)

target_link_libraries(LibSwiftOptimizer PRIVATE
  LibSwiftSIL
  swiftSILOptimizer)
set_target_properties(LibSwiftOptimizer
  PROPERTIES
  Swift_MODULE_NAME "Optimizer")
add_subdirectory(InstructionPasses)
add_subdirectory(PassManager)
add_subdirectory(FunctionPasses)
