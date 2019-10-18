// RUN: %target-swift-frontend %s -verify -o /dev/null -verify -emit-sil

@_semantics("boxtostack.mustbeonstack")
struct MustBeOnStack<T> {
  var value: T
}

class Klass {}

func testNoError() -> MustBeOnStack<Klass> {
  let x = MustBeOnStack<Klass>(value: Klass())
  return x
}

func myPrint(_ x: inout MustBeOnStack<Klass>) -> () { print(x) }

func testError() -> (() -> ()) {
  var x = MustBeOnStack<Klass>(value: Klass()) // expected-error {{Can not promote value from heap to stack due to value escaping}}
  let result = { // expected-note {{value escapes here}}
    myPrint(&x)
  }
  return result
}

func main() {
  testError()()
}

main()
