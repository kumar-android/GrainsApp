# GCam Computational Lab

This repository is a dual-target computational-photography laboratory:

- `gcam_core` is a portable C++17 CPU reference engine. It reads lossless `GCAMRAW1` Bayer frames, performs linear RAW normalization, CFA-aware bad-pixel handling, calibrated flat-field correction, luma-based alignment, noise-model-weighted robust merging, motion suppression, measurement-backed subpixel refinement, edge-aware demosaic, configurable color/tone rendering, and restrained detail processing.
- `ios_app/GCamCameraApp.xcodeproj` is an isolated native iOS target. Its AVFoundation capture adapter discovers the physical rear wide camera and runtime RAW metadata; its Metal sources provide the acceleration path. Apple frameworks are not part of the Windows CMake graph.

The visual target is a restrained, reference-driven multi-frame renderer inspired by the behavior of tuned GCam/AGC profiles. No Android binary, proprietary GCam source, generative model, or synthetic texture generation is used.

## Windows build

The portable build does not require Xcode, an Apple SDK, Swift, AVFoundation, CoreVideo, Metal, Photos, UIKit, or SwiftUI.

```powershell
cmake -S . -B build/win -G "Visual Studio 17 2022" -A x64
cmake --build build/win --config Release
ctest --test-dir build/win -C Release
```

On a Windows machine without Visual Studio, the equivalent GCC/Ninja validation is:

```powershell
cmake -S . -B build/win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/win --parallel
ctest --test-dir build/win --output-on-failure
```

The command-line tools are `build/win/Release/gcam_cli.exe` and `build/win/Release/rawpack_dump.exe` for the Visual Studio generator, or the corresponding paths directly under `build/win` for Ninja.

## RAWPACK and CLI

`GCAMRAW1` stores unpacked sensor values as little-endian `uint16` samples plus dimensions, stride, bit depth, Bayer pattern, black/white levels, exposure metadata, white balance, orientation, frame identity, lens/sensor identifiers, and optional metadata. It is linear sensor data: never demosaiced or tone-mapped.

```powershell
build/win/gcam_cli.exe generate-synthetic work/synthetic --frames 8 --width 128 --height 96
build/win/gcam_cli.exe info work/synthetic/frame_0.rawpack
build/win/gcam_cli.exe process work/synthetic --profile config/gcam_natural.xml --output work/result.ppm --diagnostics work/processing.json
build/win/gcam_cli.exe compare work/reference.ppm work/result.ppm
```

The same processor accepts a directory of `.rawpack` frames, a single frame, or a `.burst` manifest containing frame paths. `work/` is for local generated data and is ignored by Git.

## Profiles and reference matching

The XML profiles are a platform-independent tuning schema, not Android GCam XML files. They expose burst policy, noise curves, robust merge behavior, subpixel data fidelity, demosaic-related color behavior, restrained tone mapping, chroma protection, local contrast, and halo-limited sharpening. The shipped profiles are:

`gcam_natural.xml`, `gcam_hdr_plus.xml`, `gcam_high_detail.xml`, `gcam_low_light.xml`, and `gcam_neutral.xml`.

Place paired scene captures under `assets/reference/` and use the benchmark scripts in `tools/benchmark/` to produce crops and metrics. Compressed JPEGs are treated as visual references, not as exact pixel ground truth.

## iOS handoff

The iOS source tree is intentionally excluded from `CMakeLists.txt`. Open `ios_app/GCamCameraApp.xcodeproj` on macOS, select a real iPhone 16 Pro, configure signing, and build. See [docs/ios_build.md](docs/ios_build.md) and [docs/macos_ios_build.md](docs/macos_ios_build.md).

The iOS build was not executed in this Windows environment because Apple SDK/Xcode is unavailable here. The portable C++ path is the correctness oracle; Metal is an acceleration path with matching kernel responsibilities.

## Development rule

Preserve sensor information first, then use temporal measurements to suppress random noise, then render conservatively. A failure in capture or processing must return a diagnostic rather than a fabricated fallback image.
