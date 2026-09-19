# Dual-target architecture

The repository has two deliberately different build products.

```text
Windows / portable CPU oracle                 macOS / iOS native target
----------------------------------            --------------------------
CMakeLists.txt                                ios_app/GCamCameraApp.xcodeproj
  gcam_core (C++17)                           Swift capture/UI/export
  gcam_cli                                    Objective-C++ Metal bridge
  rawpack_dump                                core_metal/shaders/*.metal
  gcam_tests                                  same C++ core sources
```

The C++ engine is the shared computational model. It contains no platform headers and no camera/UI ownership. The C API in `core/include/gcam_c_api.h` is intentionally opaque and uses plain buffers and scalars so Swift does not see STL containers.

The iOS adapter owns camera discovery, RAW photo settings, CoreVideo buffer extraction, capture state, Photos output, and debug export. The Metal bridge owns GPU resource orchestration. Neither adapter is reachable from a Windows target.

## Target boundary

`gcam_core` includes only `core/include` and the C++ standard library. `CMakeLists.txt` never calls a platform SDK finder and never adds the iOS tree. The iOS project explicitly adds the core C++ sources and links Apple frameworks only in its own target settings.

Forbidden in portable core source:

- AVFoundation, CoreVideo, CoreImage, Metal, Photos, UIKit, SwiftUI
- Objective-C or Objective-C++ syntax
- Apple SDK paths or Xcode-generated intermediates

Portable responsibilities:

- RAW metadata and `GCAMRAW1` serialization
- normalization, black/white levels, saturation and CFA-aware defect correction
- flat-field model, white balance and noise model
- luma proxy, coarse/fractional translation alignment, confidence and motion estimation
- robust merge, measurement-backed subpixel refinement, demosaic
- configurable color, chroma protection, tone, local contrast and restrained sharpening
- deterministic diagnostics, CLI, tests and benchmark inputs

## iOS handoff

The Xcode target consumes the same C API and C++ source files. The only expected platform-specific work on a Mac is SDK compilation, Metal compilation, code signing, and real-device validation. The current Windows validation does not claim an iOS build.
