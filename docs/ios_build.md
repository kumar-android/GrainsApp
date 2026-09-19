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
  -scheme GCamCameraAppTests \
  -destination 'platform=iOS Simulator,name=iPhone 16 Pro' test
```

The RAW debug path saves a sequence of `GCAMRAW1` frames and a manifest so the same burst can be processed by `gcam_cli` on Windows. Final HEIF/JPEG saving is an iOS Photos concern and is not used as a substitute for the RAW path.
