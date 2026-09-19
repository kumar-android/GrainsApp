# XML tuning schema

Profiles under `config/` are a new cross-platform schema inspired by the workflow of GCam XML tuning. Android GCam XML files are not loaded or treated as dependencies.

Every profile identifies:

- `profileVersion`, `sensorTarget`, and `engineMinimumVersion`
- sensor white balance, camera matrix, radial flat-field coefficients
- burst frame limits, alignment and motion thresholds
- robust merge and edge protection parameters
- measurement-backed subpixel strength and regularization
- ISO curve points for read noise, shot coefficient, and chroma suppression
- denoise strengths (`luma`, `chroma`) with a texture-protection weight
- restrained tone and detail controls

The loader supplies documented defaults, validates ranges, sorts ISO points, and linearly interpolates between ISO calibration points. Profiles are immutable for one processing call. Changing a profile is expected to change output; the deterministic test suite checks this property.

The `denoise` values are blend ceilings, not fixed filter strengths. `chroma` blends toward a neighbour average; `luma` blends toward an edge-aware bilateral whose range scale is the ISO noise model divided by the number of merged frames. That coupling is what makes a long burst look clean without softening texture: the more frames that merged, the smaller the scale the luma pass is allowed to smooth, and `textureProtection` withholds the blend wherever the local gradient already exceeds that scale. A profile with no `isoCurve` has no noise model, so its luma pass stays inert.

The default Natural profile deliberately starts conservative. Sharpening is not used to repair bad alignment, and denoise is not used to repair bad color.
