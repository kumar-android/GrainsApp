# iOS build instructions

This target requires macOS, Xcode, an iOS SDK, and a signing team. It was not compiled in the Windows environment used for the portable validation.

1. Clone the repository on macOS.
2. Open `ios_app/GCamCameraApp.xcodeproj` in Xcode.
3. Select the `GCamCameraApp` scheme and a real iPhone 16 Pro. The simulator is suitable only for UI/non-camera checks.
4. Set the development team and bundle identifier in the target signing settings.
5. Confirm the target's header search path points to `$(SRCROOT)/../core/include` and the bridging header is `$(SRCROOT)/../ios/Processing/GCamEngineBridge.h`.
6. Build and run. The app discovers the physical rear wide camera and chooses a runtime Bayer RAW type; it does not assume a sensor resolution, bit depth, stride, Bayer order, or white level.
7. Grant camera and Photos permissions.

Command-line handoff:

```bash
xcodebuild -project ios_app/GCamCameraApp.xcodeproj \
  -scheme GCamCameraApp \
  -destination 'platform=iOS,name=iPhone 16 Pro' \
  -allowProvisioningUpdates build
```

For tests:

```bash
xcodebuild -project ios_app/GCamCameraApp.xcodeproj \
  -scheme GCamCameraApp \
  -destination 'platform=iOS Simulator,name=iPhone 16 Pro' test
```

The app's Capture preview action renders a memory-bounded on-device result from the RAW burst. The RAW debug path saves the original full-resolution sequence of `GCAMRAW1` frames and a manifest so the same burst can be processed by `gcam_cli` on Windows. Final HEIF/JPEG saving is an iOS Photos concern and is not used as a substitute for the full-resolution RAW path.

## Capture crash regression checks

Bayer RAW requests must use `AVCapturePhotoOutput.QualityPrioritization.speed`.
Using `.quality` raises `NSInvalidArgumentException` at
`capturePhoto(with:delegate:)`; Swift's `do/catch` cannot catch that exception.
The output and each request now use `.speed`. See Apple's
[capture request rules](https://developer.apple.com/documentation/avfoundation/avcapturephotooutput/capturephoto(with:delegate:)).

The shared `GCamCameraApp` scheme includes the XCTest target. The macOS CI
workflow runs its simulator tests as well as the unsigned device build. Tests
cover RAW request settings, Bayer phase preservation, padded rows, CFA detection
without DNG metadata, and invalid RAW levels. These checks need Xcode; the Windows
CMake tests exercise only the portable C++ engine.

On a physical RAW-capable iPhone:

1. Launch, allow camera access, and wait until the shutter is enabled.
2. Capture a photo and verify that processing finishes, the result has plausible
   colors, and the JPEG appears in Photos after granting write access.
3. Repeat several captures and rapid taps; only one burst should be active.
4. Interrupt capture by locking the phone, then unlock and retry if an error is
   shown. A late callback must not complete or fail a new burst.
5. Try Full RAW export and verify the full-resolution frames in the app's
   Documents directory. On a device without Bayer RAW, verify a readable error.

If a device still closes the app, retain the Xcode exception/backtrace (or iOS
JetsamEvent if it was killed for memory), the device/iOS version, and whether the
failure happened at the shutter or later in processing.
