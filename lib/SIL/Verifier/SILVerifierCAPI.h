//===--- SILVerifierCAPI.h ------------------------------------------------===//
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
///
/// C API for calling the SILVerifier from a dynamically loaded dylib.
///
//===----------------------------------------------------------------------===//

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SILVERIFIERCAPI_WEAK
#error "SILVERIFIERCAPI_WEAK defined?!"
#endif
#define SILVERIFIERCAPI_WEAK  __attribute__((weak_import))

#define SILVERIFIERCAPI_NDEBUG

/// Pass in a type erased SILModule. Inside this API, we convert back to
/// silModule and run the verifier.
extern void SILVerifierCAPI_verifySILModule(void *silModule)
  SILVERIFIERCAPI_WEAK;

/// Pass in a type erased SILFunction. Inside this API, we convert back to
/// SILFunction and run the verifier.
extern void SILVerifierCAPI_verifySILFunction(void *silFunction)
  SILVERIFIERCAPI_WEAK;

#undef SILVERIFIERCAPI_WEAK
#ifdef __cplusplus
}
#endif
