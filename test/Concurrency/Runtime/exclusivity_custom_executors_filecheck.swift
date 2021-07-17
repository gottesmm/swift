// RUN: export env SWIFT_DEBUG_RUNTIME_EXCLUSIVITY_LOGGING=1 && %target-run-simple-swift(-Xfrontend -enable-experimental-concurrency %import-libdispatch -parse-as-library) | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: executable_test

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: use_os_stdlib

// This test makes sure that we properly save/restore access when we
// synchronously launch a task from a serial executor. The access from the task
// should be merged into the already created access set while it runs and then
// unmerged afterwards.

import _Concurrency
import Dispatch

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

@inlinable
public func printStderr(_ s: String) {
    fputs(s + "\n", stderr)
}

@inline(never)
public func withExclusiveAccess<T, U>(to x: inout T, f: (inout T) -> U) -> U {
    f(&x)
}

@available(SwiftStdlib 5.5, *)
public final class MySerialExecutor : SerialExecutor {
    public init() {
        printStderr("==> MySerialExecutor: Creating MySerialExecutor!")
    }
    public static var sharedSerialExecutor = MySerialExecutor()
    public static var sharedUnownedExecutor: UnownedSerialExecutor {
        printStderr("==> MySerialExecutor: Getting Shared Unowned Executor!")
        return UnownedSerialExecutor(ordinary: sharedSerialExecutor)
    }

    public func enqueue(_ job: UnownedJob) {
        printStderr("==> MySerialExecutor: Got an enqueue!")
        // This is the exclusive access that we are going to be swizzling
        // in/out.
        withExclusiveAccess(to: &global2) { _ in
            printStderr("==> MySerialExecutor: Inside access!")
            job.runSynchronously(on: asUnownedSerialExecutor())
            printStderr("==> MySerialExecutor: Inside access after run synchronously!")
        }
        printStderr("==> MySerialExecutor: After access, after run synchronously")
    }

    public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
        printStderr("==> MySerialExecutor: Getting Unowned Executor!")
        return UnownedSerialExecutor(ordinary: self)
    }
}

/// A singleton actor whose executor is equivalent to the main
/// dispatch queue.
@available(SwiftStdlib 5.5, *)
@globalActor public final actor MyMainActor: Executor {
    public static let shared = MyMainActor()
    public let executor = MySerialExecutor()

  @inlinable
  public nonisolated var unownedExecutor: UnownedSerialExecutor {
      printStderr("==> MyMainActor: Getting unowned exector!")
      return executor.asUnownedSerialExecutor()
  }

  @inlinable
  public static var sharedUnownedExecutor: UnownedSerialExecutor {
      printStderr("==> MyMainActor: Getting shared unowned exector!")
      return MySerialExecutor.sharedUnownedExecutor
  }

  @inlinable
  public nonisolated func enqueue(_ job: UnownedJob) {
      printStderr("==> MyMainActor: enqueuing!")
      executor.enqueue(job)
  }
}

@available(SwiftStdlib 5.5, *)
actor Custom {
  var count = 0

  func report() async {
    printStderr("==> Custom: custom.count == \(count)")
    count += 1
  }
}

@available(SwiftStdlib 5.5, *)
@globalActor
struct CustomActor {
    static var shared: Custom {
        printStderr("==> CustomActor: Getting custom!")
        return Custom()
    }
}

var global: Int = 5
var global2: Int = 5

@available(SwiftStdlib 5.5, *)
@main struct Main {
    @inline(never)
    @CustomActor static func main2(_ x: inout Int) async {
        printStderr("==> Enter 'main2'")
        withExclusiveAccess(to: &global2) { _ in printStderr("==> Crash!") }
        let actor = Custom()
        await actor.report()
        await actor.report()
        await actor.report()
        printStderr("==> Exit 'main2'")
    }

    static func main() {
        do {
            withExclusiveAccess(to: &global) { _ in
                printStderr("==> Main: Front of withExclusiveAccess")
                // Task is going to start on another thread, so we aren't going
                // to have the access from global.
                let handle = Task { @MyMainActor in
                    printStderr("==> Main: In handle!")
                    withExclusiveAccess(to: &global2) { _ in printStderr("==> Crash!") }
                    await main2(&global)
                    printStderr("==> Main: All done!")
                    exit(0)
                }
                printStderr("==> Main: Before dispatch main")
                dispatchMain()
            }
        }
    }
}
