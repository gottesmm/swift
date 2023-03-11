// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-move-only -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test

@inline(never)
func printInt(_ x: Int) { print("MyInt: \(x)") }

@_moveOnly
struct FD {
    var i = 5

    deinit {
        printInt(i)
    }
}

public class FDHaver {
    var fd: FD

    init() {
        self.fd = FD()
    }
}

func main() {
    // CHECK: before 1
    print("before 1")
    do {
        // CHECK-NEXT: MyInt: 5
        let x = FD()
        let _ = x
    }
    // CHECK-NEXT: after 1
    print("after 1")

    // CHECK-NEXT: before 2
    print("before 2")
    do {
        // CHECK-NEXT: MyInt: 5
        let x = FD()
    }
    // CHECK-NEXT: after 2
    print("after 2")

    // CHECK-NEXT: before 3
    print("before 3")
    do {
        // CHECK-NEXT: MyInt: 5
        let haver = FDHaver()
    }
    // CHECK-NEXT: after 3
    print("after 3")
}

main()
