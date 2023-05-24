//===--- MoveOnlyCanonicalizer.cpp ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// A small transformation that canonicalizes SIL before running the move
/// checker upon it.
///
/// These occur sometimes due to SILGen bouncing values back/forth through
/// memory when emitting code in library evolution mode.
///
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

////////////////////////////////
// MARK: Top Level Entrypoint //
////////////////////////////////

namespace {

class MoveOnlyCanonicalizer : public SILFunctionTransform {
  void run() override {
  }
};

} // namespace

SILTransform *swift::createMoveOnlyCanonicalizer() {
  return new MoveOnlyCanonicalizer();
}
