// RUN: %target-swift-frontend -parse-as-library -import-objc-header %S/Inputs/ErrorBridging.h -module-name Swift -parse-stdlib -emit-silgen -enable-sil-ownership -enable-guaranteed-normal-arguments %s | %FileCheck %s

// REQUIRES: objc_interop

// This test checks specific codegen related to normal arguments being passed at
// +0. Eventually, it should be merged into normal SILGen tests.

/////////////////
// Fake Stdlib //
/////////////////

precedencegroup AssignmentPrecedence {
  assignment: true
}

public protocol ExpressibleByNilLiteral {
  init(nilLiteral: ())
}

protocol IteratorProtocol {
  associatedtype Element
  mutating func next() ->  Element?
}

protocol Sequence {
  associatedtype Element
  associatedtype Iterator : IteratorProtocol where Iterator.Element == Element

  func makeIterator() -> Iterator
}

enum Optional<T> {
case none
case some(T)
}

extension Optional : ExpressibleByNilLiteral {
  public init(nilLiteral: ()) {
    self = .none
  }
}

func _diagnoseUnexpectedNilOptional(_filenameStart: Builtin.RawPointer,
                                    _filenameLength: Builtin.Word,
                                    _filenameIsASCII: Builtin.Int1,
                                    _line: Builtin.Word) {
  // This would usually contain an assert, but we don't need one since we are
  // just emitting SILGen.
}

class Klass {
  init() {}
}

struct Buffer {
  var k: Klass
  init(inK: Klass) {
    k = inK
  }
}

public typealias AnyObject = Builtin.AnyObject
public typealias Void = ()

protocol Error {}

extension NSError : Error {}

public protocol _Pointer {
  var _rawValue: Builtin.RawPointer { get }
  //init(_ _rawValue: Builtin.RawPointer)
}

public struct AutoreleasingUnsafeMutablePointer<Pointee> : _Pointer {
  public let _rawValue: Builtin.RawPointer
}

///////////
// Tests //
///////////

func testNativeToErrorBridging(x: MyFoo) throws {
  try x.removeItem(for: x)
}
