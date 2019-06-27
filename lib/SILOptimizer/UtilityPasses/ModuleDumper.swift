
import SwiftBridge

@_cdecl("WrapperTransform_EntryPoint_ModuleDumper")
public func main(module: swiftbridge_sil_SILModule) {
#if DUMP_MODULE
  swiftbridge_sil_SILModule_dump(module)
#else
  print(" I AM IN SWIFT AND I AM VERIFYING THE MODULE !")
  swiftbridge_sil_SILModule_verify(module)
#endif
}
