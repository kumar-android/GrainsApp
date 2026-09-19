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
- highlight headroom in stops, and the local tone map's strength, range and recovery weights

The loader supplies documented defaults, validates ranges, sorts ISO points, and linearly interpolates between ISO calibration points. Profiles are immutable for one processing call. Changing a profile is expected to change output; the deterministic test suite checks this property.

The `tone` element carries the tone map: `localStrength` is how much of the multi-scale base compression is applied, `localRange` is the distance from the scene mean, in log-luminance, at which that compression stops, and `highlightRecovery` is how far a channel near white is pulled toward luminance so a recovered highlight reads as light rather than as a colour cast. `headroomStops` is the merge's highlight headroom: how far above the anchor exposure's own white level a frame the burst underexposed may carry a sensel before the merge clamps it. It also decides whether the burst brackets at all - with `0.0` no range above white is reported and the display white point is never moved - which is what the Neutral profile does. When a burst does carry range, the display white point (step 13 of the pipeline) is measured on the rendered plane and the recovered highlight is mapped back below white rather than clipped, so a larger value buys more recovered range at the cost of a slightly darker render.

The `denoise` values are blend ceilings, not fixed filter strengths. `chroma` blends toward a neighbour average; `luma` blends toward an edge-aware bilateral whose range scale is the ISO noise model divided by the number of merged frames. That coupling is what makes a long burst look clean without softening texture: the more frames that merged, the smaller the scale the luma pass is allowed to smooth, and `textureProtection` withholds the blend wherever the local gradient already exceeds that scale. A profile with no `isoCurve` has no noise model, so its luma pass stays inert.

The default Natural profile deliberately starts conservative. Sharpening is not used to repair bad alignment, and denoise is not used to repair bad color.
