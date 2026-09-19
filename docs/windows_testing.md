# Windows validation

The Windows path is the correctness oracle and does not require Xcode.

```powershell
cmake -S . -B build/win -G "Visual Studio 17 2022" -A x64
cmake --build build/win --config Release
ctest --test-dir build/win -C Release --output-on-failure
```

The repository also supports a GCC/Ninja validation where Visual Studio is not installed:

```powershell
cmake -S . -B build/win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/win --parallel
ctest --test-dir build/win --output-on-failure
```

The deterministic tests cover Bayer mapping, lossless RAWPACK round trips, bounded output, repeatability, C API ownership, and profile sensitivity. The synthetic CLI path generates a known Bayer scene with shot/read noise, fractional shifts, a structured edge, and a moving object.

```powershell
build/win/gcam_cli.exe generate-synthetic work/burst --frames 8 --width 256 --height 192
build/win/gcam_cli.exe process work/burst --profile config/gcam_natural.xml --output work/result.ppm --diagnostics work/processing.json
build/win/rawpack_dump.exe work/burst/frame_0.rawpack
```

The Windows CI workflow configures, builds, tests, generates a synthetic burst, processes it, and uploads the diagnostics. It never invokes an iOS target.
