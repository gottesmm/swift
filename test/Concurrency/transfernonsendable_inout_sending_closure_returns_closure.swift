// RUN: %target-swift-frontend -emit-sil -swift-version 6 -target %target-swift-5.1-abi-triple -verify %s -o /dev/null -parse-as-library

// This test verifies the closure-aware diagnostic emitted when a closure
// returned from a higher-order function with an 'inout sending' parameter
// captures a non-Sendable value derived from that parameter, putting the
// returned closure in the same region as the inout sending parameter.

// REQUIRES: concurrency

class NonSendableKlass {
  var data: NonSendableKlass? = nil
  func use() {}
}

class HasNonSendableField {
  var field: NonSendableKlass = NonSendableKlass()
}

// A higher-order function whose closure takes an 'inout sending' parameter and
// returns a closure. The returned-closure shape is what triggers the new
// closure-captures-value diagnostic.
func takeClosureReturningClosure<T>(
  _ fn: (inout sending NonSendableKlass) -> T
) -> T {
  fatalError()
}

func takeFieldClosureReturningClosure<T>(
  _ fn: (inout sending HasNonSendableField) -> T
) -> T {
  fatalError()
}

//////////////////////////////////
// MARK: Diagnosed cases        //
//////////////////////////////////

// The returned closure captures a non-Sendable value derived from the inout
// sending parameter via a 'let' binding.
func testReturnedClosureCapturesDerivedValue() {
  let _ = takeClosureReturningClosure { (state: inout sending NonSendableKlass) -> () -> Void in
    let captured = state
    return { // expected-error {{result of closure cannot be returned because it captures 'captured'}}
      captured.use() // expected-note {{returning a closure that captures 'captured' risks concurrent access to 'inout sending' parameter 'state' as caller assumes 'state' and result can be sent to different isolation domains}}
    }
  }
}

// The returned closure captures a non-Sendable stored property extracted from
// the inout sending parameter.
func testReturnedClosureCapturesStoredProperty() {
  let _ = takeFieldClosureReturningClosure { (state: inout sending HasNonSendableField) -> () -> Void in
    let extracted = state.field
    return { // expected-error {{result of closure cannot be returned because it captures 'extracted'}}
      extracted.use() // expected-note {{returning a closure that captures 'extracted' risks concurrent access to 'inout sending' parameter 'state' as caller assumes 'state' and result can be sent to different isolation domains}}
    }
  }
}

//////////////////////////////////
// MARK: Safe (no diagnostic)   //
//////////////////////////////////

// The returned closure has no captures.
func testSafeReturnedClosureNoCapture() {
  let _ = takeClosureReturningClosure { (state: inout sending NonSendableKlass) -> () -> Void in
    state = NonSendableKlass()
    return {
      // empty body
    }
  }
}

// The returned closure captures only Sendable values.
func testSafeReturnedClosureSendableCapture() {
  let _ = takeClosureReturningClosure { (state: inout sending NonSendableKlass) -> () -> Void in
    state = NonSendableKlass()
    let n = 42
    return {
      _ = n
    }
  }
}
