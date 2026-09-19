# Performance and memory

The Windows implementation is a correctness-oriented CPU reference. It keeps the algorithm explicit and deterministic so Metal can be compared against it.

The iOS handoff reserves GPU work for normalization, luma/pyramid operations, warps, merge, demosaic, tone, and detail kernels. The Objective-C++ bridge is the location for command-buffer and resource-pool management; Swift remains responsible for capture/UI/export rather than image mathematics.

The RAW capture, export, and on-device preview paths all stay at the sensor's full resolution, and the app can ask for a burst of up to 32 frames. The vector reference engine packs every frame into a normalized 16-bit plane as it arrives, so a resident burst costs two bytes per pixel per frame plus a fixed render working set; `GCamProcessor` spends its budget on frames rather than on resolution, keeps as many full-resolution frames as fit, and reduces resolution only when even two frames of the whole sensor do not fit (48 MP-class Bayer RAW).

A 32-frame burst at 12 MP is roughly 780 MiB of frame storage, so the requested count is a ceiling rather than a promise. Both `RAWBurstCapture` and `GCamProcessor` bound it by the memory that actually fits, and the result summary reports what merged (`4032x3024 - merged 12 of 32 frames`).

Resolution is the last thing to give up, because every frame has to be reduced on the same sampling grid for the reduced burst to demosaic and merge at all - and that removes the sub-pixel phase differences `estimate_alignment` and the detail recovery rely on. When a reduction is unavoidable the sampler keeps whole 2x2 Bayer cells, so every frame keeps all four CFA phases, and it averages all the same-phase sensels inside each cell it replaces rather than keeping one of them. Picking one sensel per cell would alias: the detail above the smaller grid's sampling rate would fold back in as moire, which is what made a reduced result look like a decimated RAW frame rather than a processed one. The exported `GCAMRAW1` burst is never reduced and remains the full-resolution input for Windows processing. The diagnostics schema records processing time, the burst's exposure range in stops, the recovered highlight share and an estimated peak working set.

## Measured on the Windows reference

1024x768 synthetic burst of the deterministic scene, `gcam_natural.xml`, one core, MinGW-w64:

| frames | processing | peak working set |
| --- | --- | --- |
| 1 | 0.4 s | 29 MiB |
| 8 | 0.8 s | 39 MiB |
| 32 | 2.4 s | 75 MiB |

The alignment pyramid is what keeps the 32-frame case off the floor: the earlier fixed six-pixel full-resolution search cost 139 s at 12 MP for the same burst, and clamped past its radius so the extra frames smeared instead of averaging. Matching a frame whose exposure differs from the reference costs one extra pass over its luma plane to measure the exposure ratio; a burst of one exposure measures a ratio of one and skips the pass entirely.

A 12 MP burst scales the same way - 1 frame 6.3 s / 442 MiB, 8 frames 12.3 s / 605 MiB, 32 frames 35.4 s / 1163 MiB - which is the shape the Metal path has to match.
