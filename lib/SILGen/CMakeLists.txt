add_swift_host_library(swiftSILGen STATIC
  ArgumentSource.cpp
  Cleanup.cpp
  Condition.cpp
  FormalEvaluation.cpp
  ManagedValue.cpp
  ResultPlan.cpp
  RValue.cpp
  Scope.cpp
  SwitchEnumBuilder.cpp
  SILGen.cpp
  SILGenApply.cpp
  SILGenBridging.cpp
  SILGenBuilder.cpp
  SILGenBuiltin.cpp
  SILGenConstructor.cpp
  SILGenConvert.cpp
  SILGenDecl.cpp
  SILGenDestructor.cpp
  SILGenDynamicCast.cpp
  SILGenEpilog.cpp
  SILGenExpr.cpp
  SILGenForeignError.cpp
  SILGenFunction.cpp
  SILGenGlobalVariable.cpp
  SILGenLazyConformance.cpp
  SILGenLValue.cpp
  SILGenPattern.cpp
  SILGenPoly.cpp
  SILGenProlog.cpp
  SILGenStmt.cpp
  SILGenThunk.cpp
  SILGenType.cpp)
target_link_libraries(swiftSILGen PRIVATE
  swiftSIL)

add_swift_host_library(swiftSwiftLibDemo SHARED
  REQUIRES_SWIFTC
  Lib1.swift)
set_target_properties(swiftSwiftLibDemo PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE)
target_link_options(swiftSwiftLibDemo PUBLIC "LINKER:-install_name,@rpath/libswiftSwiftLibDemo.dylib")
target_link_libraries(swiftSILGen PUBLIC swiftSwiftLibDemo)
