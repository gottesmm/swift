// RUN: %target-swift-emit-silgen %s | %FileCheck %s

struct G<Key: CaseIterable, Value> where Key: RawRepresentable, Value: Codable {
  var key: Key.RawValue
}

protocol P: CaseIterable, RawRepresentable {}

struct Value: Codable {}

enum Key: Int, P {
  case elt
}

func callee<Key: P>(_: Key.Type, _: @escaping (G<Key, Value>) -> Void) {}

// CHECK-LABEL: sil hidden [ossa] @$s029type_lowering_subst_function_A20_requirement_machine6calleryyF : $@convention(thin) () -> () {
// CHECK: function_ref @$s029type_lowering_subst_function_A20_requirement_machine6calleryyFyAA1GVyAA3KeyOAA5ValueVGcfU_ : $@convention(thin) @substituted <τ_0_0 where τ_0_0 : CaseIterable, τ_0_0 : RawRepresentable> (@in_guaranteed G<τ_0_0, Value>) -> () for <Key>
func caller() {
  callee(Key.self, { _ in })
}

REQUIRES: updating_for_owned_noescape
