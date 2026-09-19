#include "gcam_tuning.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace gcam {
namespace {

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Tuning profile: cannot open " + path);
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string attribute(const std::string& tag, const std::string& name, const std::string& fallback = {}) {
    const std::regex expression(name + R"(\s*=\s*["']([^"']*)["'])");
    std::smatch match;
    if (std::regex_search(tag, match, expression)) {
        return match[1].str();
    }
    return fallback;
}

float number(const std::string& tag, const std::string& name, float fallback) {
    const std::string value = attribute(tag, name);
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stof(value);
    } catch (const std::exception&) {
        throw std::runtime_error("Tuning profile: invalid numeric attribute " + name);
    }
}

std::uint32_t whole(const std::string& tag, const std::string& name, std::uint32_t fallback) {
    const float value = number(tag, name, static_cast<float>(fallback));
    if (value < 0.0f || value > 1000000.0f) {
        throw std::runtime_error("Tuning profile: invalid integer attribute " + name);
    }
    return static_cast<std::uint32_t>(value);
}

bool boolean(const std::string& tag, const std::string& name, bool fallback) {
    const std::string value = attribute(tag, name);
    if (value.empty()) {
        return fallback;
    }
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    throw std::runtime_error("Tuning profile: invalid boolean attribute " + name);
}

std::array<float, 9> matrix(const std::string& tag, const std::array<float, 9>& fallback) {
    const std::string value = attribute(tag, "cameraMatrix");
    if (value.empty()) return fallback;
    std::array<float, 9> result{};
    std::stringstream stream(value);
    std::string token;
    std::size_t index = 0;
    while (std::getline(stream, token, ',') && index < result.size()) {
        try {
            result[index++] = std::stof(token);
        } catch (const std::exception&) {
            throw std::runtime_error("Tuning profile: invalid cameraMatrix");
        }
    }
    if (index != result.size() || std::getline(stream, token, ',')) {
        throw std::runtime_error("Tuning profile: cameraMatrix must contain nine values");
    }
    return result;
}

template <typename T>
void validate_range(const char* name, T value, T low, T high) {
    if (value < low || value > high) {
        throw std::runtime_error(std::string("Tuning profile: ") + name + " is outside its safe range");
    }
}

} // namespace

TuningProfile default_tuning_profile() {
    TuningProfile profile;
    profile.isoCurve = {
        {32.0f, 0.0025f, 0.012f, 0.035f},
        {100.0f, 0.0040f, 0.020f, 0.050f},
        {400.0f, 0.0070f, 0.034f, 0.075f},
        {800.0f, 0.0110f, 0.050f, 0.100f},
        {1600.0f, 0.0180f, 0.075f, 0.135f}
    };
    return profile;
}

TuningProfile load_tuning_profile(const std::string& path) {
    const std::string xml = read_text(path);
    const std::regex rootExpression(R"(<gcamEmulationProfile\b[^>]*>)");
    std::smatch rootMatch;
    if (!std::regex_search(xml, rootMatch, rootExpression)) {
        throw std::runtime_error("Tuning profile: missing gcamEmulationProfile root");
    }
    const std::string root = rootMatch[0].str();
    TuningProfile profile = default_tuning_profile();
    profile.name = attribute(root, "name", profile.name);
    profile.profileVersion = static_cast<int>(whole(root, "profileVersion", static_cast<std::uint32_t>(profile.profileVersion)));
    profile.sensorTarget = attribute(root, "sensorTarget", profile.sensorTarget);
    profile.engineMinimumVersion = attribute(root, "engineMinimumVersion", profile.engineMinimumVersion);

    auto tag = [&](const char* name) {
        const std::regex expression(std::string(R"(<)" ) + name + R"(\b[^>]*/?>)");
        std::smatch match;
        return std::regex_search(xml, match, expression) ? match[0].str() : std::string{};
    };

    const std::string sensor = tag("sensor");
    const std::string burst = tag("burst");
    const std::string merge = tag("merge");
    const std::string subpixel = tag("superResolution");
    const std::string denoise = tag("denoise");
    const std::string tone = tag("tone");
    const std::string detail = tag("detail");
    const std::string color = tag("color");

    profile.cameraColorMatrix = matrix(sensor, profile.cameraColorMatrix);
    profile.lensShadingPolynomial = {
        number(sensor, "lensRadial0", profile.lensShadingPolynomial[0]),
        number(sensor, "lensRadial1", profile.lensShadingPolynomial[1]),
        number(sensor, "lensRadial2", profile.lensShadingPolynomial[2]),
        number(sensor, "lensRadial3", profile.lensShadingPolynomial[3])
    };
    profile.fallbackWhiteBalance = {
        number(sensor, "wbR", profile.fallbackWhiteBalance[0]),
        number(sensor, "wbG", profile.fallbackWhiteBalance[1]),
        number(sensor, "wbB", profile.fallbackWhiteBalance[2])
    };
    profile.targetFrames = whole(burst, "targetFrames", profile.targetFrames);
    profile.maxFrames = whole(burst, "maxFrames", profile.maxFrames);
    profile.motionThreshold = number(burst, "motionThreshold", profile.motionThreshold);
    profile.alignmentThreshold = number(burst, "alignmentThreshold", profile.alignmentThreshold);
    profile.robustHuberK = number(merge, "huberK", profile.robustHuberK);
    profile.outlierRejection = number(merge, "outlierRejection", profile.outlierRejection);
    profile.edgeProtection = number(merge, "edgeProtection", profile.edgeProtection);
    profile.enableSubpixelReconstruction = boolean(subpixel, "enabled", profile.enableSubpixelReconstruction);
    profile.subpixelStrength = number(subpixel, "strength", profile.subpixelStrength);
    profile.subpixelRegularization = number(subpixel, "regularization", profile.subpixelRegularization);
    profile.chromaDenoise = number(denoise, "chroma", profile.chromaDenoise);
    profile.lumaDenoise = number(denoise, "luma", profile.lumaDenoise);
    profile.textureProtection = number(denoise, "textureProtection", profile.textureProtection);
    profile.exposure = number(tone, "exposure", profile.exposure);
    profile.blackPoint = number(tone, "blackPoint", profile.blackPoint);
    profile.shadowCompression = number(tone, "shadow", profile.shadowCompression);
    profile.midtoneContrast = number(tone, "midtone", profile.midtoneContrast);
    profile.highlightRolloff = number(tone, "highlight", profile.highlightRolloff);
    profile.shoulder = number(tone, "shoulder", profile.shoulder);
    profile.localContrast = number(tone, "localContrast", profile.localContrast);
    profile.microcontrastAmount = number(detail, "microcontrast", profile.microcontrastAmount);
    profile.fineSharpen = number(detail, "fineSharpen", profile.fineSharpen);
    profile.midSharpen = number(detail, "midSharpen", profile.midSharpen);
    profile.sharpenThreshold = number(detail, "threshold", profile.sharpenThreshold);
    profile.haloProtection = number(detail, "haloProtection", profile.haloProtection);
    (void)color;

    const std::regex isoExpression(R"(<point\b[^>]*/?>)");
    for (std::sregex_iterator it(xml.begin(), xml.end(), isoExpression), end; it != end; ++it) {
        const std::string point = it->str();
        profile.isoCurve.push_back({
            number(point, "iso", 100.0f),
            number(point, "readNoise", 0.004f),
            number(point, "shotCoefficient", 0.02f),
            number(point, "chromaDenoise", profile.chromaDenoise)
        });
    }
    std::sort(profile.isoCurve.begin(), profile.isoCurve.end(), [](const IsoPoint& a, const IsoPoint& b) { return a.iso < b.iso; });

    validate_range("targetFrames", profile.targetFrames, 1U, 32U);
    validate_range("maxFrames", profile.maxFrames, profile.targetFrames, 64U);
    validate_range("motionThreshold", profile.motionThreshold, 0.001f, 1.0f);
    validate_range("alignmentThreshold", profile.alignmentThreshold, 0.001f, 10.0f);
    validate_range("outlierRejection", profile.outlierRejection, 0.01f, 2.0f);
    validate_range("chromaDenoise", profile.chromaDenoise, 0.0f, 0.75f);
    validate_range("lumaDenoise", profile.lumaDenoise, 0.0f, 1.0f);
    validate_range("fineSharpen", profile.fineSharpen, 0.0f, 0.75f);
    validate_range("haloProtection", profile.haloProtection, 0.0f, 1.0f);
    return profile;
}

float interpolate_iso(const TuningProfile& profile, float iso, float IsoPoint::*member) {
    if (profile.isoCurve.empty()) return 0.0f;
    if (iso <= profile.isoCurve.front().iso) return profile.isoCurve.front().*member;
    if (iso >= profile.isoCurve.back().iso) return profile.isoCurve.back().*member;
    for (std::size_t i = 1; i < profile.isoCurve.size(); ++i) {
        const IsoPoint& a = profile.isoCurve[i - 1];
        const IsoPoint& b = profile.isoCurve[i];
        if (iso <= b.iso) {
            const float t = (iso - a.iso) / std::max(1.0f, b.iso - a.iso);
            return a.*member + (b.*member - a.*member) * t;
        }
    }
    return profile.isoCurve.back().*member;
}

} // namespace gcam
