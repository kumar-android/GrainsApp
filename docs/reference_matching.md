# Reference matching workflow

Use paired captures rather than single-image claims:

1. Capture a scene with the reference GCam/AGC setup and record lighting, distance, and exposure context.
2. Capture an iPhone Bayer RAW burst and export it through the iOS debug path.
3. Process the exact exported burst on Windows with the same profile used by the iOS path.
4. Compare luminance/color distributions, edge energy, flat-region noise, highlight transitions, texture energy, and crops at 100%, 200%, 400%, and 800%.
5. Change the smallest number of XML parameters needed and rerun the full scene set.

The reference is a visual target. A compressed JPEG cannot establish exact sensor-level or pixel-level equivalence. No Android library or proprietary source is required by this workflow.
