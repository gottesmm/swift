// RUN: %target-swift-frontend -emit-sil -strict-concurrency=complete -enable-upcoming-feature RegionBasedIsolation -disable-availability-checking -verify %s -o /dev/null

// REQUIRES: concurrency
// REQUIRES: asserts

// This test validates how we handle partial applies that are isolated to a
// specific isolation domain (causing isolation crossings to occur).

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {}

actor Custom {
  var x = NonSendableKlass()
}

@globalActor
struct CustomActor {
    static var shared: Custom {
        return Custom()
    }
}

func useValue<T>(_ t: T) {}
@MainActor func transferToMain<T>(_ t: T) {}
@CustomActor func transferToCustom<T>(_ t: T) {}

/////////////////
// MARK: Tests //
/////////////////

func doSomething(_ x: NonSendableKlass, _ y: NonSendableKlass) { }

actor ProtectsNonSendable {
  var ns: NonSendableKlass = .init()

  nonisolated func testParameter(_ ns: NonSendableKlass) async {
    self.assumeIsolated { isolatedSelf in
      isolatedSelf.ns = ns // expected-warning {{task isolated value of type 'NonSendableKlass' transferred to actor-isolated context; later accesses to value could race}}
    }
  }

  // This should get the note since l is different from 'ns'.
  nonisolated func testParameterMergedIntoLocal(_ ns: NonSendableKlass) async {
    // expected-note @-1 {{value is task isolated since it is in the same region as 'ns'}}
    let l = NonSendableKlass()
    doSomething(l, ns)
    self.assumeIsolated { isolatedSelf in
      isolatedSelf.ns = l // expected-warning {{task isolated value of type 'NonSendableKlass' transferred to actor-isolated context; later accesses to value could race}}
    }
  }

  nonisolated func testLocal() async {
    let l = NonSendableKlass()

    // This is safe since we do not reuse l.
    self.assumeIsolated { isolatedSelf in
      isolatedSelf.ns = l
    }
  }

  nonisolated func testLocal2() async {
    let l = NonSendableKlass()

    // This is not safe since we use l later.
    self.assumeIsolated { isolatedSelf in
      isolatedSelf.ns = l // expected-warning {{actor-isolated closure captures value of non-Sendable type 'NonSendableKlass' from nonisolated context; later accesses to value could race}}
    }

    useValue(l) // expected-note {{access here could race}}
  }
}

func normalFunc_testLocal_1() {
  let x = NonSendableKlass()
  let _ = { @MainActor in
    print(x)
  }
}

func normalFunc_testLocal_2() {
  let x = NonSendableKlass()
  let _ = { @MainActor in
    useValue(x) // expected-warning {{main actor-isolated closure captures value of non-Sendable type 'NonSendableKlass' from nonisolated context; later accesses to value could race}}
  }
  useValue(x) // expected-note {{access here could race}}
}

// We error here since we are performing a double transfer.
//
// TODO: Add special transfer use so we can emit a double transfer error
// diagnostic.
func transferBeforeCaptureErrors() async {
  let x = NonSendableKlass()
  await transferToCustom(x) // expected-warning {{transferring value of non-Sendable type 'NonSendableKlass' from nonisolated context to global actor 'CustomActor'-isolated context}}
  let _ = { @MainActor in // expected-note {{access here could race}}
    useValue(x)
  }
}
