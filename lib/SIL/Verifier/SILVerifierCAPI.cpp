//===--- SILVerifierCAPI.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILVerifierCAPI.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILFunction.h"

/// Pass in a type erased SILModule. Inside this API, we convert back to
/// silModule and run the verifier.
extern "C"
void SILVerifierCAPI_verifySILModule(void *silModule) {
  reinterpret_cast<swift::SILModule *>(silModule)->verify();
}

/// Pass in a type erased SILFunction. Inside this API, we convert back to
/// SILFunction and run the verifier.
extern "C"
void SILVerifierCAPI_verifySILFunction(void *silFunction) {
  reinterpret_cast<swift::SILFunction *>(silFunction)->verify();
}
