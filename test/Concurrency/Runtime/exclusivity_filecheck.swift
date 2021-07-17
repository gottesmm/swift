// RUN: export env SWIFT_DEBUG_RUNTIME_EXCLUSIVITY_LOGGING=1 && %target-run-simple-swift(-Xfrontend -enable-experimental-concurrency -parse-as-library) 2>&1 | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency

// Only enabled if our stdlib has asserts enabled since the exclusivity runtime
// will only emit logging when the stdlib is compiled with asserts. This is done
// on purpose since we do not want to ship the runtime with this logging even
// possible.
//
// UNSUPPORTED: swift_stdlib_no_asserts

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// This test makes sure that:
//
// 1. Tasks have separate exclusivity sets.
// 2. Exercise the pushing/popping of access sets from tasks.

// NOTE: The cases that we are talking about handling below refer to the cases
// documented in Exclusivity.cpp.

import _Concurrency
import StdlibUnittest

var global1: Int = 5
var global2: Int = 6
var global3: Int = 7

public func nonAsync() {}

// For fputs
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
// TODO: Windowz
#endif

func printStderr(_ s: String) {
    fputs(s + "\n", stderr)
}

@available(SwiftStdlib 5.5, *)
@main
struct Runner {
    @MainActor
    @Sendable
    @inline(never)
    static func withExclusiveAccess<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
        await f(&x)
    }

    @inline(never)
    @MainActor
    static func doSomething() async { }

    @inline(never)
    @Sendable
    static func useGlobal(_ x: inout Int) { printStderr("FORCE ACCESS") }

    // Test specifically that we properly maintain different access sets when
    // using different tasks. We are not trying to specifically test any of the
    // cases here.
    //
    // CHECK-LABEL: ==> Enter 'testDifferentTasksHaveDifferentExclusivityAccessSets'
    //
    // This is our the access from await callee1(&global1). We should have only
    // one access.
    // CHECK-NEXT: Inserting new access: [[LINKEDLIST_NODE1:0x[0-9a-f]+]]
    // CHECK-NEXT: Tracking
    // CHECK-NEXT:   Access. Pointer: [[ACCESS1_PTR:0x[0-9a-f]+]]
    //
    // CHECK-NEXT: ==> Enter callee1
    //
    // Now we are inside callee1 and we hit our first suspend point to call
    // callee2. So we need to swizzle the memory. We want to check that our
    // thread local storage after we are done swizzling has as its head/tail our
    // single access and that the exclusivity runtime doesn't see any live
    // accesses. Our initial task state should be initialized to 0.
    //
    // CHECK-NEXT: Exiting Thread Local Context. Before Swizzle.
    // CHECK-NEXT:    SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:    Access. Pointer: [[ACCESS1_PTR]]. PC: {{0x[0-9a-f]+}}. AccessAction: modification
    // CHECK-NEXT: Exiting Thread Local Context. After Swizzle.
    // CHECK-NEXT:    SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[LINKEDLIST_NODE1]], [[LINKEDLIST_NODE1]])
    // CHECK-NEXT:    No Accesses.
    //
    // There are a bunch of other pushing/popping of the null case. We are
    // uninterested beyond making sure there are no more accesses.
    // CHECK-NOT: Inserting new access:
    //
    // When we enter the callee1 closure, we are going to insert the conflicting
    // access. It is from a different thread and we properly swizzled so the
    // exclusivity runtime will not see anything. There are no await statements
    // so no swizzling happens until we return fraom callee1 closure.
    //
    // CHECK: ==> Enter callee1 closure
    // CHECK-NEXT: Inserting new access: [[LINKEDLIST_NODE2:0x[a-z0-9]+]]
    // CHECK-NEXT: Tracking!
    // CHECK-NEXT:     Access. Pointer: [[ACCESS2_PTR:0x[a-z0-9]+]].
    // CHECK-NEXT: ==> Enter callee2
    // CHECK-NEXT: ==> Exit callee2
    // CHECK-NEXT: Removing access: [[LINKEDLIST_NODE2]]
    // CHECK-NEXT: ==> Exit callee1 closure
    //
    // Now that we have exited from callee1 closure, we swizzle back in our
    // original access list from the task.
    //
    // CHECK-NEXT: Entering Thread Local Context. Before Swizzle.
    // CHECK-NEXT:     SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[LINKEDLIST_NODE1]], [[LINKEDLIST_NODE1]])
    // CHECK-NEXT:     No Accesses.
    // CHECK-NEXT: Entering Thread Local Context. After Swizzle.
    // CHECK-NEXT:     SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:     Access. Pointer: [[ACCESS1_PTR]].
    //
    // We then do a bunch of switching back and forth due to unoptimized
    // hopping. Just make sure we do not insert any more accesses.
    //
    // CHECK-NOT: Inserting new access:
    //
    // CHECK: ==> Exit callee1
    // CHECK-NEXT: Removing access: [[LINKEDLIST_NODE1]]
    //
    // CHECK: ==> Exit 'testDifferentTasksHaveDifferentExclusivityAccessSets'
    @MainActor static func testDifferentTasksHaveDifferentExclusivityAccessSets() async {
        let callee2 = { @MainActor (_ x: inout Int) -> Void in
            printStderr("==> Enter callee2")
            printStderr("==> Exit callee2")
        }

        // We add an inline never here to make sure that we do not eliminate
        // the dynamic access after inlining.
        @MainActor
        @inline(never)
        func callee1(_ x: inout Int) async -> () {
            printStderr("==> Enter callee1")
            // This task is what prevents this example from crashing.
            let handle = Task { @MainActor in
                printStderr("==> Enter callee1 closure")
                // Second access. Different Task so it is ok.
                callee2(&global1)
                printStderr("==> Exit callee1 closure")
            }
            await handle.value
            printStderr("==> Exit callee1")
        }

        printStderr("==> Enter 'testDifferentTasksHaveDifferentExclusivityAccessSets'")
        // First access begins here.
        await callee1(&global1)
        printStderr("==> Exit 'testDifferentTasksHaveDifferentExclusivityAccessSets'")
    }

    // Now that we have tested our tests with various numbers of accesses,
    // lets make specific tests for each case in Exclusivity.cpp.
    //
    // Case 1: (F, F, F) - No Live Accesses at Task Start, No Live Sync
    // Accesses When Push, No Live Task Accesses when pop.
    //
    // This case is the case where we do not have any accesses in our code
    // at all or if the task cleans up the tasks before it awaits again. We
    // test the task cleanup case.
    //
    // CHECK-LABEL : ==> Enter 'testCase1'
    //
    // CHECK-NOT: Inserting new access:
    //
    // CHECK: ==> Enter callee1 Closure
    // CHECK-NEXT: Inserting new access: [[LINKED_LIST1:0x[0-9a-z]+]]
    // CHECK-NEXT:   Tracking!
    // CHECK-NEXT:         Access. Pointer: [[ACCESS1:0x[0-9a-z]+]]. PC:
    // CHECK-NEXT: Inserting new access: [[LINKED_LIST2:0x[0-9a-z]+]]
    // CHECK-NEXT:   Tracking!
    // CHECK-NEXT:         Access. Pointer: [[ACCESS2:0x[0-9a-z]+]]. PC:
    // CHECK-NEXT:         Access. Pointer: [[ACCESS1]]. PC:
    // CHECK-NEXT: Inserting new access: [[LINKED_LIST3:0x[0-9a-z]+]]
    // CHECK-NEXT:   Tracking!
    // CHECK-NEXT:         Access. Pointer: [[ACCESS3:0x[0-9a-z]+]]. PC:
    // CHECK-NEXT:         Access. Pointer: [[ACCESS2]]. PC:
    // CHECK-NEXT:         Access. Pointer: [[ACCESS1]]. PC:
    // CHECK-NEXT: ==> Enter callee2
    // CHECK-NEXT: ==> Exit callee2
    // CHECK-NEXT: Removing access: [[LINKED_LIST3]]
    // CHECK-NEXT: Removing access: [[LINKED_LIST2]]
    // CHECK-NEXT: Removing access: [[LINKED_LIST1]]
    //
    // At this point, we make sure that we properly exit the thread, finishing our test.
    // CHECK-NEXT: Exiting Thread Local Context. Before Swizzle. Storage:
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         No Accesses.
    // CHECK-NEXT: Exiting Thread Local Context. After Swizzle. Storage:
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         No Accesses.
    //
    // CHECK: ==> Exit 'testCase1'
    @MainActor static func testCase1() async {
        @inline(never)
        @Sendable func callee2(_ x: inout Int, _ y: inout Int, _ z: inout Int) -> Void {
            printStderr("==> Enter callee2")
            printStderr("==> Exit callee2")
        }

        // We add an inline never here to make sure that we do not eliminate
        // the dynamic access after inlining.
        @MainActor
        @inline(never)
        func callee1() async -> () {
            printStderr("==> Enter callee1")
            let handle = Task { @MainActor in
                printStderr("==> Enter callee1 Closure")

                // These accesses end before we await in the task.
                do {
                    callee2(&global1, &global2, &global3)
                }
                let handle2 = Task { @MainActor in
                    printStderr("==> Enter handle2!")
                    printStderr("==> Exit handle2!")
                }
                await handle2.value
                printStderr("==> Exit callee1 Closure")
            }
            await handle.value
            printStderr("==> Exit callee1")
        }

        printStderr("==> Enter 'testCase1'")
        await callee1()
        printStderr("==> Exit 'testCase1'")
    }


    // Case 2: (F, F, T). In case 2, our task does not start with a live access
    // and nothing from the outside synchronous context, but does pop with a new
    // access.
    //
    // We use a suspend point and a withExclusiveAccess(to:) to test this.
    // CHECK-LABEL: ==> Enter 'testCase2'
    // CHECK: Exiting Thread Local Context. Before Swizzle. Storage: [[TASK0:0x[0-9a-z]+]]
    // CHECK: Exiting Thread Local Context. After Swizzle. Storage: [[TASK0]]
    // CHECK: Entering Thread Local Context. Before Swizzle. Storage: [[TASK:0x[0-9a-z]+]]
    // CHECK: Entering Thread Local Context. After Swizzle. Storage: [[TASK]]
    // CHECK: Exiting Thread Local Context. Before Swizzle. Storage: [[TASK]]
    // CHECK: Exiting Thread Local Context. After Swizzle. Storage: [[TASK]]
    //
    // No accesses when we start.
    // CHECK: Entering Thread Local Context. Before Swizzle. Storage: [[TASK]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         No Accesses.
    // CHECK-NEXT: Entering Thread Local Context. After Swizzle. Storage: [[TASK]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         No Accesses.
    // CHECK-NEXT: ==> Inner Handle
    // CHECK-NEXT: Inserting new access: [[LLNODE:0x[0-9a-z]+]]
    // CHECK-NEXT:   Tracking!
    // CHECK-NEXT:         Access. Pointer: [[ACCESS:0x[0-9a-z]+]]. PC:
    // CHECK-NEXT: Exiting Thread Local Context. Before Swizzle. Storage: [[TASK]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         Access. Pointer: [[ACCESS]]. PC:
    // CHECK-NEXT: Exiting Thread Local Context. After Swizzle. Storage: [[TASK]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[LLNODE]], [[LLNODE]])
    // CHECK-NEXT:         No Accesses.
    //
    // CHECK: ==> Exit 'testCase2'
    @MainActor static func testCase2() async {
        printStderr("==> Enter 'testCase2'")

        let handle = Task { @MainActor in
            printStderr("==> Inner Handle")
            await withExclusiveAccess(to: &global1) { @MainActor (x: inout Int) async -> Void in
                let innerTaskHandle = Task { @MainActor in
                    printStderr("==> End Inner Task Handle")
                }
                await innerTaskHandle.value
                printStderr("==> After")
            }
            printStderr("==> Inner Handle: After exclusive access")
        }
        await handle.value
        printStderr("==> After exclusive access")
        let handle2 = Task { @MainActor in
            printStderr("==> Enter handle2!")
            printStderr("==> Exit handle2!")
        }
        await handle2.value
        printStderr("==> Exit 'testCase2'")
    }

    // Case 5: (T,F,F). To test case 5, we use with exclusive access to to
    // create an exclusivity scope that goes over a suspension point. We are
    // interesting in the case where we return after the suspension point. That
    // push/pop is going to have our outer task bring in state and end it.
    //
    // CHECK-LABEL: ==> Enter 'testCase5'
    // CHECK-NEXT: Inserting new access: [[LLNODE:0x[a-z0-9]+]]
    // CHECK-NEXT: Tracking!
    // CHECK-NEXT: Access. Pointer: [[ACCESS:0x[a-z0-9]+]]
    // CHECK-NEXT: Exiting Thread Local Context. Before Swizzle. Storage: [[TASK:0x[a-z0-9]+]]
    // CHECK-NOT: Removing access:
    // CHECK: ==> End Inner Task Handle
    // CHECK: ==> After
    // CHECK-NEXT: Removing access: [[LLNODE]]
    // CHECK-NEXT: ==> After exclusive access
    // CHECK-NEXT: Exiting Thread Local Context. Before Swizzle. Storage: [[TASK]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         No Accesses.
    // CHECK-NEXT: Exiting Thread Local Context. After Swizzle. Storage: [[TASK]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         No Accesses.
    //
    // CHECK: ==> Exit 'testCase5'
    @MainActor static func testCase5() async {
        printStderr("==> Enter 'testCase5'")

        await withExclusiveAccess(to: &global1) { @MainActor (x: inout Int) async -> Void in
            let innerTaskHandle = Task { @MainActor in
                printStderr("==> End Inner Task Handle")
            }
            await innerTaskHandle.value
            printStderr("==> After")
        }
        printStderr("==> After exclusive access")
        let handle2 = Task { @MainActor in
            printStderr("==> Enter handle2!")
            printStderr("==> Exit handle2!")
        }
        await handle2.value
        printStderr("==> Exit 'testCase5'")
    }

    // Case 6: (T, F, T). In case 6, our task starts with live accesses and is
    // popped with live accesses. There are no sync accesses.
    //
    // We test this by looking at the behavior of the runtime after we finish
    // executing handle2. In this case, we use the logging to validate that the
    // original task1 accesses are brought back in and when we pop one last time
    // as we await again, we properly restore the state.
    //
    // CHECK-LABEL: ==> Enter 'testCase6'
    // CHECK-NEXT: Inserting new access: [[TASK1_LLNODE:0x[0-9a-z]+]]
    // CHECK-NEXT: Tracking!
    // CHECK-NEXT: Access. Pointer: [[ACCESS1:0x[0-9a-z]+]].
    // CHECK-NEXT: ==> Enter callee1
    // CHECK-NEXT: Exiting Thread Local Context. Before Swizzle. Storage: [[TASK1:0x[0-9a-z]+]]
    // CHECK: Exiting Thread Local Context. After Swizzle. Storage: [[TASK1]]
    // CHECK-NEXT: SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[TASK1_LLNODE]], [[TASK1_LLNODE]])
    //
    // CHECK: Entering Thread Local Context. After Swizzle. Storage: [[TASK2:0x[0-9a-z]+]]
    // We do a shuffle here.
    // CHECK: Entering Thread Local Context. After Swizzle. Storage: [[TASK2]]
    // CHECK-NEXT: SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT: No Accesses.
    // CHECK-NEXT: ==> Enter callee1 Closure
    // CHECK: ==> Exit callee1 Closure
    //
    // At this point, TASK2 has died and we will not get further notificatiosn
    // of it.  But we don't really care about it. What we care about is that we
    // properly restore task1's state.
    // CHECK-NEXT: Entering Thread Local Context. Before Swizzle. Storage: [[TASK1]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess):
    // CHECK-NEXT:         No Accesses.
    // CHECK-NEXT: Entering Thread Local Context. After Swizzle. Storage: [[TASK1]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         Access. Pointer:
    //
    // We then importantly actually serialize this state again allowing us to
    // fully test case 2 when we resume from handle2!
    //
    // CHECK: ==> Enter handle2!
    // CHECK-NEXT: ==> Exit handle2!
    // CHECK-NEXT: Entering Thread Local Context. Before Swizzle. Storage: [[TASK1]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[TASK1_LLNODE]], [[TASK1_LLNODE]])
    // CHECK-NEXT:         No Accesses.
    // CHECK-NEXT: Entering Thread Local Context. After Swizzle. Storage: [[TASK1]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         Access. Pointer: [[ACCESS1]]. PC:
    //
    // Then we shuffle back.
    // CHECK-NEXT: Exiting Thread Local Context. Before Swizzle. Storage: [[TASK1]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
    // CHECK-NEXT:         Access. Pointer: [[ACCESS1]]. PC:
    // CHECK-NEXT: Exiting Thread Local Context. After Swizzle. Storage: [[TASK1]]
    // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[TASK1_LLNODE]], [[TASK1_LLNODE]])
    // CHECK-NEXT:         No Accesses.
    //
    // CHECK: ==> Exit 'testCase6'
    @MainActor static func testCase6() async {
        let callee2 = { @MainActor (_ x: inout Int) -> Void in
            printStderr("==> Enter callee2")
            printStderr("==> Exit callee2")
        }

        // We add an inline never here to make sure that we do not eliminate
        // the dynamic access after inlining.
        @MainActor
        @inline(never)
        func callee1(_ x: inout Int) async -> () {
            printStderr("==> Enter callee1")
            // This task is what prevents this example from crashing.
            let handle = Task { @MainActor in
                printStderr("==> Enter callee1 Closure")
                // Second access. Different Task so it is ok.
                await withExclusiveAccess(to: &global1) {
                    await callee2(&$0)
                }
                //await callee2(&global1)
                printStderr("==> Exit callee1 Closure")
            }
            await handle.value
            printStderr("==> callee1 after first await")
            // Force an await here so we can see that we properly swizzle.
            let handle2 = Task { @MainActor in
                printStderr("==> Enter handle2!")
                printStderr("==> Exit handle2!")
            }
            await handle2.value
            printStderr("==> Exit callee1")
        }

        printStderr("==> Enter 'testCase6'")
        // First access begins here.
        await callee1(&global1)
        printStderr("==> Exit 'testCase6'")
    }

    @MainActor static func main() async {
        await testDifferentTasksHaveDifferentExclusivityAccessSets()
        await testCase1()
        await testCase2()
        await testCase5()
        await testCase6()
    }
}
