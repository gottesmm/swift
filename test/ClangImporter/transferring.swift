// RUN: %target-swift-frontend -swift-version 6 -disable-availability-checking -emit-sil -o /dev/null %s -parse-as-library -enable-experimental-feature TransferringArgsAndResults -verify -import-objc-header %S/Inputs/transferring.h

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

@MainActor func transferToMain<T>(_ t: T) async {}
func useValue<T>(_ t: T) {}

/////////////////
// MARK: Tests //
/////////////////

func methodTestTransferringResult() async {
  let x = MyType()
  let y = x.getTransferringResult()
  await transferToMain(x)
  useValue(y)
}

func methodTestTransferringArg() async {
  let x = MyType()
  let s = NSObject()
  let _ = x.getResultWithTransferringArgument(s)  // expected-error {{binding of non-Sendable type 'NSObject' accessed after being transferred}}
  useValue(s) // expected-note {{access here could race}}
}

// Make sure we just ignore the swift_attr if it is applied to something like a
// class.
func testDoesntMakeSense() {
  let _ = DoesntMakeSense()
}

func funcTestTransferringResult() async {
  let x = NSObject()
  let y = testCallGlobalWithTransferringResult(x)
  await transferToMain(x)
  useValue(y)

  // Just to show that without the transferring param, we generate diagnostics.
  let x2 = NSObject()
  let y2 = testCallGlobalWithResult(x2)
  await transferToMain(x2) // expected-error {{transferring 'x2' may cause a race}}
  // expected-note @-1 {{transferring disconnected 'x2' to main actor-isolated callee could cause races in between callee main actor-isolated and local nonisolated uses}}
  useValue(y2) // expected-note {{access here could race}}
}

func funcTestTransferringArg() async {
  let x = NSObject()
  testCallGlobalWithTransferringArg(x) // expected-error {{binding of non-Sendable type 'NSObject' accessed after being transferred}}
  useValue(x) // expected-note {{access here could race}}
}
