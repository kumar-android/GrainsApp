#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gcam {

enum class BayerPattern : std::uint8_t {
    RGGB = 0,
    BGGR = 1,
    GRBG = 2,
    GBRG = 3,
    Unknown = 255
};

enum class PixelChannel : std::uint8_t { Red = 0, Green = 1, Blue = 2 };

struct RawMetadata {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowStrideBytes = 0;
    std::uint16_t bitDepth = 16;
    BayerPattern bayer = BayerPattern::Unknown;
    float blackLevel = 0.0f;
    float whiteLevel = 65535.0f;
    float iso = 100.0f;
    float exposureTimeSeconds = 0.0f;
    float aperture = 0.0f;
    float colorTemperatureKelvin = 0.0f;
    std::array<float, 3> whiteBalance = {1.0f, 1.0f, 1.0f};
    std::int32_t orientation = 1;
    std::int64_t timestampUnixMicros = 0;
    std::uint32_t frameIndex = 0;
    std::string lensIdentifier;
    std::string sensorIdentifier;
    std::string optionalMetadata;
};

struct RawFrame {
    RawMetadata metadata;
    std::vector<std::uint16_t> pixels;

    bool valid() const noexcept;
    std::uint16_t at(std::uint32_t x, std::uint32_t y) const noexcept;
    std::uint16_t& at(std::uint32_t x, std::uint32_t y) noexcept;
};

struct RGBImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> pixels;

    RGBImage() = default;
    RGBImage(std::uint32_t w, std::uint32_t h) : width(w), height(h), pixels(static_cast<std::size_t>(w) * h * 3U, 0.0f) {}

    float& at(std::uint32_t x, std::uint32_t y, PixelChannel c) noexcept {
        return pixels[(static_cast<std::size_t>(y) * width + x) * 3U + static_cast<std::size_t>(c)];
    }
    float at(std::uint32_t x, std::uint32_t y, PixelChannel c) const noexcept {
        return pixels[(static_cast<std::size_t>(y) * width + x) * 3U + static_cast<std::size_t>(c)];
    }
};

struct AlignmentEstimate {
    float dx = 0.0f;
    float dy = 0.0f;
    float rotationRadians = 0.0f;
    float scale = 1.0f;
    float confidence = 0.0f;
    float residualError = 0.0f;
    bool accepted = true;
};

struct ProcessingDiagnostics {
    std::string profileName;
    std::uint32_t inputFrames = 0;
    std::uint32_t acceptedFrames = 0;
    std::uint32_t rejectedFrames = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float meanAlignmentConfidence = 0.0f;
    float meanAlignmentResidual = 0.0f;
    float motionFraction = 0.0f;
    float mergeConfidence = 0.0f;
    float peakMemoryMiB = 0.0f;
    double processingMilliseconds = 0.0;
    std::vector<AlignmentEstimate> alignments;
    std::string error;
};

struct ProcessResult {
    RGBImage image;
    ProcessingDiagnostics diagnostics;
};

const char* bayer_pattern_name(BayerPattern pattern) noexcept;
bool parse_bayer_pattern(const std::string& value, BayerPattern& pattern) noexcept;
PixelChannel bayer_channel(BayerPattern pattern, std::uint32_t x, std::uint32_t y) noexcept;

} // namespace gcam
