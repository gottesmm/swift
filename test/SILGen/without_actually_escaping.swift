// RUN: %target-swift-frontend -emit-silgen -enable-sil-ownership %s | %FileCheck %s

var escapeHatch: Any = 0

// CHECK-LABEL: sil hidden @$S25without_actually_escaping9letEscape1fyycyyXE_tF
func letEscape(f: () -> ()) -> () -> () {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $@noescape @callee_guaranteed () -> ()):
  // TODO: Use a canary wrapper instead of just copying the nonescaping value
  // CHECK: [[ESCAPABLE_COPY:%.*]] = copy_value [[ARG]]
  // CHECK: [[CONVERT:%.*]] = convert_function [[ESCAPABLE_COPY]]
  // CHECK: [[BORROWED_CONVERT:%.*]] = begin_borrow [[CONVERT]]
  // CHECK: [[SUB_CLOSURE:%.*]] = function_ref @
  // CHECK: [[RESULT:%.*]] = apply [[SUB_CLOSURE]]([[BORROWED_CONVERT]])
  // CHECK: destroy_value [[CONVERT]]
  // CHECK: return [[RESULT]]
  return withoutActuallyEscaping(f) { return $0 }
}
