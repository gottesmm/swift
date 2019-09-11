// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

// REQUIRES: objc_interop

import Foundation

class Canary {
  var x: Int

  deinit { print("dead \(x)") }
  init(_ x: Int) { self.x = x }
  func chirp() { print("\(x)") }
}

func main() {
  print("making array")

  var b: NSArray = NSArray(objects: [Canary(1), Canary(2), Canary(3)], count: 3)

  print("iterating array")

  // CHECK-DAG: 1
  for x in b {
    (x as! Canary).chirp()
    break
  }

  // CHECK-DAG: exiting
  print("exiting")
}

autoreleasepool {
  main()
}
