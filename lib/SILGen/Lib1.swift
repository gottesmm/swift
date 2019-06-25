
class ImASwiftClass {
  init() {}
}

@_cdecl("inject_into_swift")
public func main() {
  let p = ImASwiftClass()
  print(p)
  print("Hello from Swift!")
}
