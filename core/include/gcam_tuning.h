#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gcam {

struct IsoPoint {
    float iso = 100.0f;
    float readNoise = 0.004f;
    float shotCoefficient = 0.02f;
    float chromaDenoise = 0.06f;
};

struct TuningProfile {
    std::string name = "Natural";
    int profileVersion = 1;
    std::string sensorTarget = "runtime Bayer sensor";
    std::string engineMinimumVersion = "0.1.0";

    std::uint32_t targetFrames = 8;
    std::uint32_t maxFrames = 12;
    float motionThreshold = 0.055f;
    float alignmentThreshold = 0.12f;
    bool enableSubpixelReconstruction = true;
    float subpixelStrength = 0.22f;
    float subpixelRegularization = 0.10f;

    float robustHuberK = 1.5f;
    float outlierRejection = 0.35f;
    float edgeProtection = 0.85f;
    float chromaDenoise = 0.06f;
    float lumaDenoise = 0.35f;
    float textureProtection = 0.90f;

    std::array<float, 9> cameraColorMatrix = {
        1.55f, -0.32f, -0.23f,
        -0.12f, 1.22f, -0.10f,
        -0.08f, -0.25f, 1.33f
    };
    std::array<float, 4> lensShadingPolynomial = {1.0f, 0.0f, 0.0f, 0.0f};

    float exposure = 0.0f;
    float blackPoint = 0.0f;
    float shadowCompression = 0.10f;
    float midtoneContrast = 1.02f;
    float highlightRolloff = 0.72f;
    float shoulder = 0.24f;
    float localContrast = 0.035f;
    float localToneStrength = 0.55f;
    // Log-luminance distance from the scene mean at which local compression stops.
    float localToneRange = 0.85f;
    float highlightRecovery = 0.45f;
    // How far above the reference frame's white level the merge is allowed to carry a
    // pixel that a darker frame in the same burst still resolved. The recovered range is
    // rolled off into the last stretch below white instead of being clipped flat.
    float highlightHeadroomStops = 1.5f;

    float fineSharpen = 0.10f;
    float midSharpen = 0.035f;
    float sharpenThreshold = 0.035f;
    float haloProtection = 0.90f;
    float microcontrastAmount = 0.045f;
    float microcontrastRadius = 1.5f;
    float microcontrastThreshold = 0.025f;
    float microcontrastTextureProtection = 0.90f;

    std::array<float, 3> fallbackWhiteBalance = {2.0f, 1.0f, 1.6f};
    std::vector<IsoPoint> isoCurve;
};

TuningProfile default_tuning_profile();
TuningProfile load_tuning_profile(const std::string& path);
float interpolate_iso(const TuningProfile& profile, float iso, float IsoPoint::*member);

} // namespace gcam
