// swift-tools-version:5.0

import PackageDescription

// Simple package that pulls in SwiftBridge as a module so one can play around
// with code in swift in a playground or in xcode.
var products: [Product] = []
products.append(.library(name: "SwiftBridgeOptPasses", type: .static, targets: ["SwiftBridgeOptPasses"]))

var targets: [Target] = []
targets.append(.systemLibrary(name: "SwiftBridge", path: "include/swift-c/SwiftBridge"))
targets.append(.target(name: "SwiftBridgeOptPasses", dependencies: ["SwiftBridge"], path: "lib/SILOptimizer/UtilityPasses", sources: ["ModuleDumper.swift"]))

let p = Package(
  name: "swiftbridge",
  products: products,
  targets: targets,
  swiftLanguageVersions: [.v5]
)
