# Performance and memory

The Windows implementation is a correctness-oriented CPU reference. It keeps the algorithm explicit and deterministic so Metal can be compared against it.

The iOS handoff reserves GPU work for normalization, luma/pyramid operations, warps, merge, demosaic, tone, and detail kernels. The Objective-C++ bridge is the location for command-buffer and resource-pool management; Swift remains responsible for capture/UI/export rather than image mathematics.

At full sensor resolution, an iOS implementation should move from the reference vectors to tiled buffers with overlap, release per-frame intermediates after merge, and keep autorelease-pool boundaries around large captures. Resolution must not be reduced merely to hide a memory problem. The diagnostics schema already records processing time and an estimated peak working set.
