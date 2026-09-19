# macOS/iOS handoff

The intended later workflow is:

```text
clone repository
open ios_app/GCamCameraApp.xcodeproj
select a real iPhone 16 Pro
configure signing
build and run
capture + export a RAW burst
process the exported burst on Windows for A/B comparison
```

The portable core, XML profiles, bridging header, Metal kernels, Swift capture adapter, app target, and test target are already in the repository. No source copying or processing-engine rewrite should be required.

No macOS CI is claimed by the Windows build. A future macOS workflow can run `xcodebuild test` and compile the Metal sources once a macOS runner and signing policy are available.
