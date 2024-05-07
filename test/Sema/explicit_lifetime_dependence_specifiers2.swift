// RUN: %target-typecheck-verify-swift -disable-availability-checking -enable-experimental-feature NonescapableTypes
// REQUIRES: asserts
// REQUIRES: nonescapable_types

struct AnotherBufferView : ~Escapable, BitwiseCopyable {
  let ptr: UnsafeRawBufferPointer
  @_unsafeNonescapableResult
  init(_ ptr: UnsafeRawBufferPointer) {
    self.ptr = ptr
  }
}

struct BufferView : ~Escapable {
  let ptr: UnsafeRawBufferPointer
  init(_ bv: borrowing AnotherBufferView) -> _borrow(bv) Self {
    self.ptr = bv.ptr
    return self
  }
}
