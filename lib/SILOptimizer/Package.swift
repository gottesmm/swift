// swift-tools-version:5.0
// The swift-tools-version declares the minimum version of Swift required to build this package.

// This is a Swift Package for building Swift SILOptimizer against the
// SwiftBridge module in Xcode via swiftpm. If this builds, then one knows the
// code will also build as part of Swift's cmake.
//
// GENERATING AN XCODEPROJECT
// --------------------------
//
// To generate an xcodeproject for this, run:
//
// ```
// swift package generate-xcodeproj --xcconfig-overrides Package.xcconfig
// ```
//
// This is important to ensure that Xcode can find the SwiftBridge module. This
// seems to allow for compilation to work, but for some reason SourceKit cannot
// generate a Swift representation of the module.

import PackageDescription

let package = Package(
  name: "SILOptimizer",
  products: [
    // Products define the executables and libraries produced by a package, and make them visible to other packages.
    .library(
      name: "SILOptimizer",
      type: .static,
      targets: ["SILOptimizer"]),
 ],
  targets: [
    // Targets are the basic building blocks of a package. A target can define a module or a test suite.
    // Targets can depend on other targets in this package, and on products in packages which this package depends on.
    .target(
      name: "SILOptimizer",
      path: "UtilityPasses",
      sources: ["ModuleDumper.swift"])
  ]
)
