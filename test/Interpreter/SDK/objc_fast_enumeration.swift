// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

// REQUIRES: objc_interop

import Foundation

final class Canary {
  var x: Int

  deinit { print("dead \(x)") }
  @inline(never)
  init(_ x: Int) { self.x = x }
  @inline(never)
  func chirp() { print("\(x)") }
}

func makingArray() {
  print("making array")
}

func iteratingArray() {
  print("iterating array")
}

func exiting() {
  print("exiting!")
}

func main() {
  makingArray()

  var b: NSArray = NSArray(object: Canary(1))

  iteratingArray()

  // CHECK-DAG: 1
  for x in b {
    (x as! Canary).chirp()
    break
  }

  // CHECK-DAG: exiting
  exiting()
}

autoreleasepool {
  main()
}
