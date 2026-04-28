// RUN: %target-swift-frontend -swift-version 6 -Xllvm -sil-regionbasedisolation-force-use-of-typed-errors=src-only -emit-sil -o /dev/null %s -verify -verify-additional-prefix ni- -target %target-swift-5.1-abi-triple
// RUN: %target-swift-frontend -swift-version 6 -Xllvm -sil-regionbasedisolation-force-use-of-typed-errors=src-only -emit-sil -o /dev/null %s -verify -verify-additional-prefix ni-ns- -target %target-swift-5.1-abi-triple -enable-upcoming-feature NonisolatedNonsendingByDefault

// REQUIRES: concurrency
// REQUIRES: asserts
// REQUIRES: swift_feature_NonisolatedNonsendingByDefault

// This test verifies that -sil-regionbasedisolation-force-use-of-typed-errors=src-only
// only forces type-based diagnostics for the src side of cross-isolation merge
// emitters. Send emitters use InferRole::Default and are unaffected, so they
// continue to produce name-based diagnostics.

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {}

actor MyActor {}

@MainActor func transferToMain<T>(_ t: T) async {}
func transferToSendingParam<T>(_ x: sending T) {}
func useNonSendable(_ a: NonSendableKlass, _ b: NonSendableKlass) {}

/////////////////
// MARK: Tests //
/////////////////

func simpleUseAfterFree() async {
  let x = NonSendableKlass()
  await transferToMain(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  print(x) // expected-note {{access can happen concurrently}}
}

func isolatedClosureTest() async {
  let x = NonSendableKlass()
  let _ = { @MainActor in
      print(x) // expected-error {{sending 'x' risks causing data races}}
      // expected-note @-1 {{'x' is captured by a main actor-isolated closure. main actor-isolated uses in closure may race against later nonisolated uses}}
  }
  print(x) // expected-note {{access can happen concurrently}}
}

func sendingError() async {
  let x = NonSendableKlass()
  transferToSendingParam(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{'x' used after being passed as a 'sending' parameter; Later uses could race}}
  print(x) // expected-note {{access can happen concurrently}}
}

extension MyActor {
  func testNonSendableCaptures(sc: NonSendableKlass) {
    Task {
      _ = self
      _ = sc

      Task { [sc,self] in
        _ = self
        _ = sc

        Task { // expected-error {{passing closure as a 'sending' parameter risks causing data races between 'self'-isolated code and concurrent execution of the closure}}
          _ = sc // expected-note {{closure captures 'self'-isolated 'sc'}}
        }

        Task { // expected-error {{passing closure as a 'sending' parameter risks causing data races between 'self'-isolated code and concurrent execution of the closure}}
          _ = sc // expected-note {{closure captures 'self'-isolated 'sc'}}
        }
      }
    }
  }
}

@MainActor
func sendingTransferNonSendableError(_ x: NonSendableKlass) {
  transferToSendingParam(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'x' is passed as a 'sending' parameter; Uses in callee may race with later main actor-isolated uses}}
}

func sendingTransferNonSendableError(_ x: NonSendableKlass) async {
  await transferToMain(x) // expected-error {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and task-isolated uses}}
}

///////////////////////////////////////////
// MARK: Cross-Isolation Merge Tests     //
///////////////////////////////////////////

@MainActor
struct MainActorStruct {
  var x: NonSendableKlass
  var y: NonSendableKlass

  // Assign merge: src uses type (binding), dst uses name
  nonisolated init(assign val: NonSendableKlass) {
    self.x = val // expected-error {{assigning a value of type 'NonSendableKlass' to main actor-isolated 'self.x' risks causing data races}}
    // expected-note @-1 {{a value of type 'NonSendableKlass' could become accessible to main actor-isolated code despite remaining accessible to code in the current task}}
    self.y = NonSendableKlass()
  }

  // NonisolatedFunction merge: src uses type (binding), dst uses name
  nonisolated init(nonisolatedFunc val: NonSendableKlass) {
    self.x = NonSendableKlass()
    self.y = NonSendableKlass()
    useNonSendable(self.x, val) // expected-error {{passing a value of type 'NonSendableKlass' and main actor-isolated 'self.x' as arguments to global function 'useNonSendable' risks causing data races}}
    // expected-note @-1 {{'self.x' could begin referencing a value of type 'NonSendableKlass' allowing concurrent access to a value of type 'NonSendableKlass' by main actor-isolated code and code in the current task}}
  }
}
