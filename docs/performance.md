# Performance and memory

The Windows implementation is a correctness-oriented CPU reference. It keeps the algorithm explicit and deterministic so Metal can be compared against it.

The iOS handoff reserves GPU work for normalization, luma/pyramid operations, warps, merge, demosaic, tone, and detail kernels. The Objective-C++ bridge is the location for command-buffer and resource-pool management; Swift remains responsible for capture/UI/export rather than image mathematics.

The RAW capture, export, and on-device preview paths all stay at the sensor's full resolution, and the app can ask for a burst of up to 32 frames. The vector reference engine packs every frame into a normalized 16-bit plane as it arrives, so a resident burst costs two bytes per pixel per frame plus a fixed render working set; `GCamProcessor` spends its budget on frames rather than on resolution, keeps as many full-resolution frames as fit, and reduces resolution only when even two frames of the whole sensor do not fit (48 MP-class Bayer RAW).

A 32-frame burst at 12 MP is roughly 780 MiB of frame storage, so the requested count is a ceiling rather than a promise. Both `RAWBurstCapture` and `GCamProcessor` bound it by the memory that actually fits, and the result summary reports what merged (`4032x3024 - merged 12 of 32 frames`).

Resolution is the last thing to give up because reducing every frame of a burst on the same sampling grid removes the sub-pixel phase differences that `estimate_alignment` and the merge rely on. A reduced burst still merges, but only averages frames that carry the same decimated detail, which is why a reduced path looked like an unprocessed single RAW frame. When a reduction is unavoidable the sampler keeps whole 2x2 Bayer cells, so every frame keeps all four CFA phases and still demosaics. The exported `GCAMRAW1` burst is never reduced and remains the full-resolution input for Windows processing. The diagnostics schema records processing time and an estimated peak working set.
