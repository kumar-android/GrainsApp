# Implementation map

The repository follows the requested dependency order:

| Stage | Current location | Windows validation |
|---|---|---|
| Portable metadata / RAW frame | `core/include`, `core/src/pipeline.cpp` | unit tests |
| RAWPACK | `core/src/rawpack.cpp`, `tools/rawpack_dump` | lossless round trip |
| Normalization and calibration | `core/src/pipeline.cpp`, XML sensor section | synthetic frames |
| Alignment and motion | `core/src/pipeline.cpp` | deterministic diagnostics |
| Robust merge | `core/src/pipeline.cpp` | synthetic burst processing |
| Detail, demosaic, color, tone | `core/src/pipeline.cpp` | bounded deterministic output |
| C API | `core/include/gcam_c_api.h`, `core/src/c_api.cpp` | C API test |
| CLI / benchmarks | `tools/gcam_cli`, `tools/benchmark` | generated RAW burst |
| Metal path | `core_metal/shaders`, `ios/Metal` | CPU oracle on Windows |
| RAW capture / export | `ios/Camera`, `ios/Capture`, `ios/Export` | macOS/device only |
| Native app | `ios_app/GCamCameraApp.xcodeproj`, `ios/UI` | macOS/device only |

The remaining validation that cannot happen on Windows is actual AVFoundation RAW availability, iPhone 16 Pro memory behavior, Metal numerical agreement, HEIF/Photos integration, and on-device capture state timing. Those are explicitly marked as macOS/iOS work rather than hidden behind Windows conditionals.
