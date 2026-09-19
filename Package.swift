// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "GCamComputationalLab",
    platforms: [.iOS(.v17)],
    products: [
        .library(name: "GCamPortableSettings", targets: ["GCamPortableSettings"])
    ],
    targets: [
        .target(name: "GCamPortableSettings", path: "ios/Support")
    ]
)
