# Portable RAW pipeline

The CPU reference path is intentionally linear until display rendering:

1. Validate runtime width, height, stride, bit depth, white/black levels and one of the four Bayer patterns.
2. Unpack the `uint16` sensor samples and subtract black level without gamma or sRGB conversion.
3. Replace only isolated CFA outliers when same-colour neighbours agree; leave structured bright detail intact.
4. Apply the profile's bounded radial flat-field polynomial and one burst white-balance estimate.
5. Pack each frame into a normalized 16-bit plane with a green-preferred luma proxy built on demand, so a burst costs two bytes per pixel per frame instead of two float planes.
6. Pick the sharpest candidate as reference, search translation on the luma proxy, then refine in quarter-pixel increments.
7. Estimate motion from post-alignment disagreement. Moving samples remain available but receive a low temporal weight.
8. Merge using an ISO-interpolated read/shot-noise variance and two robust Huber passes. Saturated samples are excluded when an unsaturated observation exists. The Huber scale is the noise model itself, so grain is averaged rather than mistaken for motion.
9. Run conservative data-fidelity back-projection from aligned observations when `enableSubpixelReconstruction` is enabled. It is not generative super-resolution and cannot create texture unsupported by the burst.
10. Demosaic using directional green and chroma interpolation with edge-aware fallback.
11. Apply the configurable camera matrix, then an edge-aware luma bilateral whose range scale is the post-merge noise model divided by the accepted frame count, then mild chroma-only spatial suppression, scene-referred tone, local contrast and halo-limited detail. The luma pass is what removes the grain the merge leaves behind; its scale shrinks as frames grow, so a 32-frame burst is not over-smoothed.
12. Emit a linear floating-point RGB result to the API; PPM export applies only the final sRGB encoding.

`GCAMRAW1` is a transfer/debug format, not a final image format. It never stores JPEG-derived pixels, demosaiced samples, or tone-mapped data.

## Error policy

Malformed frames, incompatible dimensions/Bayer patterns, invalid profiles, and an empty accepted set are hard errors. The CLI and C API report them; they do not return an unrelated fallback image.
