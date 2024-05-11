// RUN: %target-swift-frontend -emit-sil -parse-as-library -disable-availability-checking -strict-concurrency=complete -enable-experimental-feature TransferringArgsAndResults -verify %s -o /dev/null

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

class Klass {}

struct NonSendableStruct {
  var first = Klass()
  var second = Klass()
}

class KlassWithNonSendableStructPair {
  var ns1: NonSendableStruct
  var ns2: (NonSendableStruct, NonSendableStruct)

  init() {
    ns1 = NonSendableStruct()
    ns2 = (ns1, ns1)
  }
}

final class FinalKlassWithNonSendableStructPair {
  var ns1: NonSendableStruct
  var ns2: (NonSendableStruct, NonSendableStruct)

  init() {
    ns1 = NonSendableStruct()
    ns2 = (ns1, ns1)
  }
}

func useValue<T>(_ t: T) {}
func getAny() -> Any { fatalError() }

actor Custom {
}

@globalActor
struct CustomActor {
    static var shared: Custom {
        return Custom()
    }
}

@MainActor func transferToMain<T>(_ t: T) {}
@CustomActor func transferToCustom<T>(_ t: T) {}

func transferArg(_ x: transferring Klass) {
}

func transferArgWithOtherParam(_ x: transferring Klass, _ y: Klass) {
}

func transferArgWithOtherParam2(_ x: Klass, _ y: transferring Klass) {
}

func twoTransferArg(_ x: transferring Klass, _ y: transferring Klass) {}

@MainActor var globalKlass = Klass()

/////////////////
// MARK: Tests //
/////////////////

func testSimpleTransferLet() {
  let k = Klass()
  transferArg(k) // expected-warning {{sending 'k' risks causing data races}}
  // expected-note @-1 {{'k' used after being passed as a transferring parameter}}
  useValue(k) // expected-note {{access can happen concurrently}}
}

func testSimpleTransferVar() {
  var k = Klass()
  k = Klass()
  transferArg(k) // expected-warning {{sending 'k' risks causing data races}}
  // expected-note @-1 {{'k' used after being passed as a transferring parameter}}
  useValue(k) // expected-note {{access can happen concurrently}}
}

func testSimpleTransferUseOfOtherParamNoError() {
  let k = Klass()
  let k2 = Klass()
  transferArgWithOtherParam(k, k2)
  useValue(k2)
}

func testSimpleTransferUseOfOtherParamNoError2() {
  let k = Klass()
  let k2 = Klass()
  transferArgWithOtherParam2(k, k2)
  useValue(k)
}

@MainActor func transferToMain2(_ x: transferring Klass, _ y: Klass, _ z: Klass) async {

}

// TODO: How to test this?
func testNonStrongTransferDoesntMerge() async {
}

//////////////////////////////////
// MARK: Transferring Parameter //
//////////////////////////////////

func testTransferringParameter_canTransfer(_ x: transferring Klass, _ y: Klass) async {
  await transferToMain(x)
  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and task-isolated uses}}
}

func testTransferringParameter_cannotTransferTwice(_ x: transferring Klass, _ y: Klass) async {
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  // TODO: We should not error on this since we are transferring to the same place.
  await transferToMain(x) // expected-note {{access can happen concurrently}}
}

func testTransferringParameter_cannotUseAfterTransfer(_ x: transferring Klass, _ y: Klass) async {
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  useValue(x) // expected-note {{access can happen concurrently}}
}

actor MyActor {
  var field = Klass()

  func canTransferWithTransferringMethodArg(_ x: transferring Klass, _ y: Klass) async {
    await transferToMain(x)
    await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
    // expected-note @-1 {{sending actor-isolated 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and actor-isolated uses}}
    // expected-note @-2 {{'y' is inferred to be actor-isolated since it is in the same region as actor-isolated 'y' due to specified region merge path}}
  }

  func getNormalErrorIfTransferTwice(_ x: transferring Klass) async {
    await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
    // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local actor-isolated uses}}
    await transferToMain(x) // expected-note {{access can happen concurrently}}
  }

  func getNormalErrorIfUseAfterTransfer(_ x: transferring Klass) async {
    await transferToMain(x)  // expected-warning {{sending 'x' risks causing data races}}
    // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local actor-isolated uses}}
    useValue(x) // expected-note {{access can happen concurrently}}
  }

  // After assigning into the actor, we can still use x in the actor as long as
  // we don't transfer it.
  func assignTransferringIntoActor(_ x: transferring Klass) async {
    field = x
    useValue(x)
  }

  // Once we assign into the actor, we cannot transfer further.
  func assignTransferringIntoActor2(_ x: transferring Klass) async {
    field = x
    await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
    // expected-note @-1 {{sending 'self'-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and 'self'-isolated uses}}
  }
}

@MainActor func canAssignTransferringIntoGlobalActor(_ x: transferring Klass) async {
  globalKlass = x
}

@MainActor func canAssignTransferringIntoGlobalActor2(_ x: transferring Klass) async {
  globalKlass = x
  // TODO: This is incorrect! transferring should be independent of @MainActor.
  await transferToCustom(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending main actor-isolated 'x' to global actor 'CustomActor'-isolated global function 'transferToCustom' risks causing data races between global actor 'CustomActor'-isolated and main actor-isolated uses}}
}

@MainActor func canAssignTransferringIntoGlobalActor3(_ x: transferring Klass) async {
  await transferToCustom(globalKlass) // expected-warning {{sending main actor-isolated value of type 'Klass' with later accesses to global actor 'CustomActor'-isolated context risks causing data races}}
}

func canTransferAssigningIntoLocal(_ x: transferring Klass) async {
  let _ = x
  await transferToMain(x)
}

func canTransferAssigningIntoLocal2(_ x: transferring Klass) async {
  let _ = x
  await transferToMain(x)
  // We do not error here since we just load the value and do not do anything
  // with it.
  //
  // TODO: We should change let _ = x so that it has a move_value '_' or
  // something like that. It will also help move checking as well.
  let _ = x
}

func canTransferAssigningIntoLocal2a(_ x: transferring Klass) async {
  let _ = x
  await transferToMain(x)
  // We do not error here since we just load the value and do not do anything
  // with it.
  //
  // TODO: We should change let _ = x so that it has a move_value '_' or
  // something like that. It will also help move checking as well.
  _ = x
}

func canTransferAssigningIntoLocal3(_ x: transferring Klass) async {
  let _ = x
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  let y = x // expected-note {{access can happen concurrently}}
  _ = y
}

//////////////////////////////////////
// MARK: Transferring is "var" like //
//////////////////////////////////////

// Assigning into a transferring parameter is a merge.
func assigningIsAMerge(_ x: transferring Klass) async {
  let y = Klass()

  x = y

  // We can still transfer y since x is disconnected.
  await transferToMain(y)
}

func assigningIsAMergeError(_ x: transferring Klass) async {
  let y = Klass()

  x = y

  // We can still transfer y since x is disconnected.
  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
}

func assigningIsAMergeAny(_ x: transferring Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  x = y

  await transferToMain(y)
}

func assigningIsAMergeAnyError(_ x: transferring Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  x = y

  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
}

func canTransferAfterAssign(_ x: transferring Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  // y is transferred into x.
  await transferToMain(x)

  x = y

  useValue(x)
}

func canTransferAfterAssignButUseIsError(_ x: transferring Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  // y is transferred into x.
  x = y

  // TODO: This should refer to the transferring parameter.
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  useValue(x) // expected-note {{access can happen concurrently}}
}

func assignToEntireValueEliminatesEarlierTransfer(_ x: transferring Any) async {
  // Ok, this is disconnected.
  let y = getAny()

  useValue(x)

  // Transfer x
  await transferToMain(x)

  // y is transferred into x. This shouldn't error.
  x = y

  useValue(x)
}

func mergeDoesNotEliminateEarlierTransfer(_ x: transferring NonSendableStruct) async {


  // Ok, this is disconnected.
  let y = Klass()

  useValue(x)

  // Transfer x
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  // y is assigned into a field of x.
  x.first = y // expected-note {{access can happen concurrently}}

  useValue(x)
}

func mergeDoesNotEliminateEarlierTransfer2(_ x: transferring NonSendableStruct) async {
  // Ok, this is disconnected.
  let y = Klass()

  useValue(x)

  // Transfer x
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}

  x.first = y  // expected-note {{access can happen concurrently}}
}

func doubleArgument() async {
  let x = Klass()
  twoTransferArg(x, x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{'x' used after being passed as a transferring parameter}}
  // expected-note @-2 {{access can happen concurrently}}
}

func testTransferSrc(_ x: transferring Klass) async {
  let y = Klass()
  await transferToMain(y) // expected-warning {{sending 'y' risks causing data races}}
  // expected-note @-1 {{sending 'y' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and local nonisolated uses}}
  x = y // expected-note {{access can happen concurrently}}
}

func testTransferOtherParam(_ x: transferring Klass, y: Klass) async {
  x = y
}

func testTransferOtherParamTuple(_ x: transferring Klass, y: (Klass, Klass)) async {
  x = y.0
}

func taskIsolatedError(_ x: @escaping @MainActor () async -> ()) {
  func fakeInit(operation: transferring @escaping () async -> ()) {}

  fakeInit(operation: x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{task-isolated 'x' is passed as a transferring parameter; Uses in callee may race with later task-isolated uses}}
}

@MainActor func actorIsolatedError(_ x: @escaping @MainActor () async -> ()) {
  func fakeInit(operation: transferring @escaping () async -> ()) {}

  // TODO: This needs to say actor-isolated.
  fakeInit(operation: x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'x' is passed as a transferring parameter; Uses in callee may race with later main actor-isolated uses}}
}

// Make sure we error here on only the second since x by being assigned a part
// of y becomes task-isolated
func testMergeWithTaskIsolated(_ x: transferring Klass, y: Klass) async {
  await transferToMain(x)
  x = y
  await transferToMain(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending task-isolated 'x' to main actor-isolated global function 'transferToMain' risks causing data races between main actor-isolated and task-isolated uses}}
}

@MainActor func testMergeWithActorIsolated(_ x: transferring Klass, y: Klass) async {
  x = y // expected-note {{0. 'y' merged into 'x' here}}
  await transferToCustom(x) // expected-warning {{sending 'x' risks causing data races}}
  // expected-note @-1 {{sending main actor-isolated 'x' to global actor 'CustomActor'-isolated global function 'transferToCustom' risks causing data races between global actor 'CustomActor'-isolated and main actor-isolated uses}}
  // expected-note @-2 {{'x' is inferred to be main actor-isolated since it is in the same region as main actor-isolated 'y' due to specified region merge path}}
}
