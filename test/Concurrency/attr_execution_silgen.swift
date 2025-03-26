// RUN: %target-swift-emit-silgen %s -target %target-swift-5.1-abi-triple -enable-experimental-feature ExecutionAttribute | %FileCheck %s

// REQUIRES: asserts
// REQUIRES: concurrency
// REQUIRES: swift_feature_ExecutionAttribute

////////////////////////
// MARK: Declarations //
////////////////////////

@execution(caller)
func globalCallerFunc() async -> () {}

@execution(concurrent)
func globalConcurrentFunc() async -> () {}

class NonSendableKlass {
  init() {}
}
class SendableKlass : @unchecked Sendable {
  init() {}
}

func globalActorConversions2(_ x: @escaping @execution(concurrent) (SendableKlass) async -> (),
                             _ y: @escaping @execution(caller) (SendableKlass) async -> ()) async {
  // We shouldn't allow for this conversion.
  //let v3: @MainActor (SendableKlass) async -> Void = x
  //await v3(SendableKlass())
  //let v4: @MainActor (SendableKlass) async -> Void = y
  //await v4(SendableKlass())
  let v5: @execution(concurrent) (SendableKlass) async -> Void = y
  await v5(SendableKlass())
}
