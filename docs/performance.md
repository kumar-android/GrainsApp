# Performance and memory

The Windows implementation is a correctness-oriented CPU reference. It keeps the algorithm explicit and deterministic so Metal can be compared against it.

The iOS handoff reserves GPU work for normalization, luma/pyramid operations, warps, merge, demosaic, tone, and detail kernels. The Objective-C++ bridge is the location for command-buffer and resource-pool management; Swift remains responsible for capture/UI/export rather than image mathematics.

At full sensor resolution, the iOS app keeps the RAW capture/export path lossless but uses a bounded, Bayer-phase-preserving preview scale for the current vector reference path. This prevents an 8-frame full-resolution burst from exceeding device memory while the tiled Metal path is completed. The exported `GCAMRAW1` burst is never reduced and remains the full-resolution input for Windows processing. The diagnostics schema records processing time and an estimated peak working set.
