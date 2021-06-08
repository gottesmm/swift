# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for Swift project authors

add_swift_host_library(LibSwiftSIL
  STATIC
  PURE_SWIFT
  Argument.swift
  BasicBlock.swift
  Builder.swift
  Function.swift
  GlobalVariable.swift
  Instruction.swift
  Location.swift
  Operand.swift
  Registration.swift
  Type.swift
  Utils.swift
  StackList.swift
  Value.swift)

target_link_libraries(LibSwiftSIL PRIVATE swiftSIL)
set_target_properties(LibSwiftSIL
  PROPERTIES
  Swift_MODULE_NAME "SIL")
