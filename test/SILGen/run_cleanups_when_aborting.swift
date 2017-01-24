// RUN: %target-swift-frontend -emit-silgen -o - %s | %FileCheck %s

// This test makes sure that we are running cleanups in exceptional situations.

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

class C {}

//===----------------------------------------------------------------------===//
// Tests
//===----------------------------------------------------------------------===//

// Make sure that we properly emit cleanups before calling
// _getDiagnoseUnexpectedNilOptional.
// CHECK-LABEL: sil hidden @_TF4main50checkCleanupsAfterGetDiagnoseUnexpectedNilOptionalFT1xGSqSi__Si : $@convention(thin) (Optional<Int>) -> Int {
// CHECK: bb0([[ARG:%.*]] : $Optional<Int>):
// CHECK:   [[ALLOC_INIT_FUN:%.*]] = function_ref @{{.*}} : $@convention(method) (@thick C.Type) -> @owned C
// CHECK:   [[META:%.*]] = metatype $@thick C.Type
// CHECK:   [[C:%.*]] = apply [[ALLOC_INIT_FUN]]([[META]])
// CHECK:   switch_enum [[ARG]] : $Optional<Int>, case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]], default [[DEFAULT_BB:bb[0-9]+]]

// CHECK: [[NONE_BB]]:
// CHECK:   destroy_value [[C]]
// CHECK:   [[ASSERT_FUN:%.*]] = function_ref @_TF{{.*diagnoseUnexpectedNilOptional.*}} : $@convention(thin)
// CHECK:   apply [[ASSERT_FUN]]({{.*}}) : $@convention(thin)
// CHECK-NEXT:   unreachable

// CHECK: [[DEFAULT_BB]]:
// CHECK:   [[RESULT:%.*]] = unchecked_enum_data [[ARG]]
// CHECK:   destroy_value [[C]]
// CHECK:   return [[RESULT]]
// CHECK: } // end sil function '_TF4main50checkCleanupsAfterGetDiagnoseUnexpectedNilOptionalFT1xGSqSi__Si'
func checkCleanupsAfterGetDiagnoseUnexpectedNilOptional(x: Int?) -> Int {
  let c = C()
  return x!
}

