//===--- InternalOptions.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/InternalOptions.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace sil;

bool swift::sil::DestructureLargeTuples = false;
unsigned swift::sil::DestructureLargeTupleThreshold = 7;

static llvm::cl::opt<bool, true> DestructureLargeTuplesOpt(
    "sil-destructure-large-tuples",
    llvm::cl::desc(
        "Pass tuples with more than -sil-destructure-large-tuples-threshold "
        "elements without destructuring"),
    llvm::cl::Hidden,
    llvm::cl::location(DestructureLargeTuples));
static llvm::cl::opt<unsigned, true> DestructureLargeTupleThresholdOpt(
    "sil-destructure-large-tuples-threshold",
    llvm::cl::desc(
        "Threshold after which DestructureLargeTuples become destructured"),
    llvm::cl::Hidden,
    llvm::cl::location(DestructureLargeTupleThreshold));
