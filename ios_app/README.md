# Native target membership

`GCamCameraApp.xcodeproj` contains two targets:

- `GCamCameraApp`: SwiftUI wrapper, AVFoundation RAW capture, RAWPACK debug export, Photos output, C API bridge, C++ CPU source, Objective-C++ Metal bridge, and Metal kernels.
- `GCamCameraAppTests`: XCTest target for iOS-only integration checks.

The portable C++ source references are explicit in the project file and point back to `../core`. The Windows CMake target does not read this project file.
