# Performance and memory

The Windows implementation is a correctness-oriented CPU reference. It keeps the algorithm explicit and deterministic so Metal can be compared against it.

The iOS handoff reserves GPU work for normalization, luma/pyramid operations, warps, merge, demosaic, tone, and detail kernels. The Objective-C++ bridge is the location for command-buffer and resource-pool management; Swift remains responsible for capture/UI/export rather than image mathematics.

The RAW capture, export, and on-device preview paths all stay at the sensor's full resolution. The vector reference engine cannot hold an unlimited burst on device, so `GCamProcessor` spends its fixed working-set budget on frames rather than on resolution: it keeps as many full-resolution frames as fit, and only reduces resolution when even two frames of the whole sensor do not fit (48 MP-class Bayer RAW).

Resolution is the last thing to give up because reducing every frame of a burst on the same sampling grid removes the sub-pixel phase differences that `estimate_alignment` and the merge rely on. A reduced burst still merges, but only averages frames that carry the same decimated detail, which is why a reduced path looked like an unprocessed single RAW frame. When a reduction is unavoidable the sampler keeps whole 2x2 Bayer cells, so every frame keeps all four CFA phases and still demosaics. The exported `GCAMRAW1` burst is never reduced and remains the full-resolution input for Windows processing. The diagnostics schema records processing time and an estimated peak working set.
