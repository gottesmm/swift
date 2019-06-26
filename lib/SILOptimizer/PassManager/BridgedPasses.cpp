//===--- BridgedPasses.cpp ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift-c/SwiftBridge/SILOptimizerBridgedTypes.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

#define PASS(Id, Tag, Description)
#define BRIDGED_PASS(Id, Tag, Description)                                     \
  extern "C" {                                                                 \
  void WrapperTransform_EntryPoint_##Id(swiftbridge_sil_SILModule mod);        \
  }                                                                            \
                                                                               \
  namespace {                                                                  \
                                                                               \
  class WrapperTransform_##Id : public SILModuleTransform {                    \
    void run() override {                                                      \
      swiftbridge_sil_SILModule wrapper;                                       \
      wrapper.cxxImpl = (void *)getModule();                                   \
      WrapperTransform_EntryPoint_##Id(wrapper);                               \
    }                                                                          \
  };                                                                           \
  }                                                                            \
                                                                               \
  SILTransform *swift::create##Id() { return new WrapperTransform_##Id(); }
#include "swift/SILOptimizer/PassManager/Passes.def"
