#include "gcam_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#ifndef GCAM_ENGINE_VERSION
#define GCAM_ENGINE_VERSION "0.1.0"
#endif

namespace gcam {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

float smoothstep(float edge0, float edge1, float value) {
    const float t = clamp01((value - edge0) / std::max(1e-6f, edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

bool in_bounds(std::uint32_t width, std::uint32_t height, int x, int y) {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
}

// The merged plane: one normalized [0,1] sample per sensel, in float because the
// merge accumulates into it.
struct NormalizedFrame {
    RawMetadata metadata;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> linear;
};

constexpr float kNormalizedScale = 65535.0f;

float raw_value(const NormalizedFrame& frame, int x, int y) {
    if (!in_bounds(frame.width, frame.height, x, y)) return 0.0f;
    return frame.linear[static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)];
}

float packed_value(const PackedFrame& frame, int x, int y) {
    if (!in_bounds(frame.width, frame.height, x, y)) return 0.0f;
    return static_cast<float>(frame.samples[static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)]) / kNormalizedScale;
}

// Alignment reads luma, and a full float plane per frame would double a burst that
// is already storing 16-bit planes for every frame. Rebuilding luma on demand costs
// one cheap pass per frame and keeps the burst footprint at two bytes per pixel.
std::vector<float> packed_luma(const PackedFrame& frame) {
    std::vector<float> luma(frame.samples.size(), 0.0f);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            if (bayer_channel(frame.metadata.bayer, x, y) == PixelChannel::Green) {
                luma[static_cast<std::size_t>(y) * frame.width + x] = packed_value(frame, static_cast<int>(x), static_cast<int>(y));
            } else {
                float sum = 0.0f;
                int count = 0;
                for (const auto offset : {std::pair<int, int>{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
                    const int nx = static_cast<int>(x) + offset.first;
                    const int ny = static_cast<int>(y) + offset.second;
                    if (in_bounds(frame.width, frame.height, nx, ny) && bayer_channel(frame.metadata.bayer, static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny)) == PixelChannel::Green) {
                        sum += packed_value(frame, nx, ny);
                        ++count;
                    }
                }
                luma[static_cast<std::size_t>(y) * frame.width + x] = count > 0 ? sum / static_cast<float>(count) : packed_value(frame, static_cast<int>(x), static_cast<int>(y));
            }
        }
    }
    return luma;
}

template <typename Plane>
float sample_bilinear_scaled(const Plane& pixels, float scale, std::uint32_t width, std::uint32_t height, float x, float y) {
    if (width == 0 || height == 0) return 0.0f;
    x = std::max(0.0f, std::min(static_cast<float>(width - 1U), x));
    y = std::max(0.0f, std::min(static_cast<float>(height - 1U), y));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, static_cast<int>(width - 1U));
    const int y1 = std::min(y0 + 1, static_cast<int>(height - 1U));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    auto at = [&](int px, int py) {
        return static_cast<float>(pixels[static_cast<std::size_t>(py) * width + static_cast<std::size_t>(px)]) * scale;
    };
    const float top = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

float sample_bilinear(const std::vector<float>& pixels, std::uint32_t width, std::uint32_t height, float x, float y) {
    return sample_bilinear_scaled(pixels, 1.0f, width, height, x, y);
}

float sample_bilinear(const std::vector<std::uint16_t>& pixels, std::uint32_t width, std::uint32_t height, float x, float y) {
    return sample_bilinear_scaled(pixels, 1.0f / kNormalizedScale, width, height, x, y);
}

std::array<float, 3> usable_white_balance(const RawFrame& frame, const TuningProfile& profile) {
    const auto wb = frame.metadata.whiteBalance;
    if (wb[0] > 0.01f && wb[1] > 0.01f && wb[2] > 0.01f) return wb;
    return profile.fallbackWhiteBalance;
}


float gradient_energy(const std::vector<float>& luma, std::uint32_t width, std::uint32_t height) {
    if (width < 3 || height < 3) return 0.0f;
    double sum = 0.0;
    std::uint64_t count = 0;
    for (std::uint32_t y = 1; y + 1 < height; y += 2) {
        for (std::uint32_t x = 1; x + 1 < width; x += 2) {
            const float gx = sample_bilinear(luma, width, height, static_cast<float>(x + 1U), static_cast<float>(y)) - sample_bilinear(luma, width, height, static_cast<float>(x - 1U), static_cast<float>(y));
            const float gy = sample_bilinear(luma, width, height, static_cast<float>(x), static_cast<float>(y + 1U)) - sample_bilinear(luma, width, height, static_cast<float>(x), static_cast<float>(y - 1U));
            sum += std::sqrt(gx * gx + gy * gy);
            ++count;
        }
    }
    return count == 0 ? 0.0f : static_cast<float>(sum / static_cast<double>(count));
}

AlignmentEstimate estimate_alignment(const std::vector<float>& referenceLuma, const std::vector<float>& candidateLuma, std::uint32_t width, std::uint32_t height, const TuningProfile& profile) {
    AlignmentEstimate best;
    float bestError = std::numeric_limits<float>::max();
    const int searchRadius = 6;
    const int stride = width > 256 ? 4 : 2;
    const int margin = searchRadius + 3;
    // The reference samples are identical for every candidate shift, and integer
    // shifts inside the margin never leave the frame, so gathering them once turns
    // each candidate comparison into a direct luma read instead of a bilinear fetch.
    // Sample values and summation order are unchanged by this gather.
    struct LumaSample {
        int x;
        int y;
        float value;
    };
    std::vector<LumaSample> grid;
    grid.reserve((static_cast<std::size_t>(width) / static_cast<std::size_t>(stride) + 1U) *
        (static_cast<std::size_t>(height) / static_cast<std::size_t>(stride) + 1U));
    for (int y = margin; y + margin < static_cast<int>(height); y += stride) {
        for (int x = margin; x + margin < static_cast<int>(width); x += stride) {
            grid.push_back(LumaSample{x, y, sample_bilinear(referenceLuma, width, height, static_cast<float>(x), static_cast<float>(y))});
        }
    }
    const std::size_t gridCount = grid.size();
    for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
        for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
            double error = 0.0;
            for (const LumaSample& sample : grid) {
                const std::size_t index = static_cast<std::size_t>(sample.y + dy) * width + static_cast<std::size_t>(sample.x + dx);
                const float difference = candidateLuma[index] - sample.value;
                error += static_cast<double>(difference * difference);
            }
            const float normalizedError = gridCount == 0 ? 1.0f : static_cast<float>(error / static_cast<double>(gridCount));
            if (normalizedError < bestError) {
                bestError = normalizedError;
                best.dx = static_cast<float>(dx);
                best.dy = static_cast<float>(dy);
            }
        }
    }

    // A small deterministic fractional refinement provides the subpixel warp
    // used by the merge without pretending that unsupported texture exists.
    float refinedError = bestError;
    const float fractions[] = {-0.5f, -0.25f, 0.0f, 0.25f, 0.5f};
    for (float fy : fractions) {
        for (float fx : fractions) {
            const float dx = best.dx + fx;
            const float dy = best.dy + fy;
            double error = 0.0;
            for (const LumaSample& sample : grid) {
                const float difference = sample_bilinear(candidateLuma, width, height, static_cast<float>(sample.x) + dx, static_cast<float>(sample.y) + dy) - sample.value;
                error += static_cast<double>(difference * difference);
            }
            const float normalizedError = gridCount == 0 ? 1.0f : static_cast<float>(error / static_cast<double>(gridCount));
            if (normalizedError < refinedError) {
                refinedError = normalizedError;
                best.dx = dx;
                best.dy = dy;
            }
        }
    }
    best.residualError = refinedError;
    best.confidence = clamp01(1.0f - std::sqrt(refinedError) * 5.0f);
    best.accepted = refinedError <= profile.alignmentThreshold && best.confidence >= 0.10f;
    return best;
}

float robust_weight(float residual, float variance, const TuningProfile& profile) {
    // The ISO noise model supplies the scale. `motionThreshold` floors it so that
    // a low-noise burst is not rejected as outliers by an optimistic model.
    const float scale = std::max(profile.motionThreshold, std::sqrt(std::max(1e-6f, variance)));
    const float threshold = profile.robustHuberK * scale;
    if (residual <= threshold) return 1.0f;
    return threshold / std::max(threshold, residual);
}

float interpolate_missing(const NormalizedFrame& frame, int x, int y, PixelChannel channel) {
    if (!in_bounds(frame.width, frame.height, x, y)) return 0.0f;
    const std::uint32_t ux = static_cast<std::uint32_t>(x);
    const std::uint32_t uy = static_cast<std::uint32_t>(y);
    if (bayer_channel(frame.metadata.bayer, ux, uy) == channel) return raw_value(frame, x, y);

    auto directional = [&](int dx, int dy, float& gradient, bool& found) {
        const int x0 = x - dx;
        const int y0 = y - dy;
        const int x1 = x + dx;
        const int y1 = y + dy;
        if (in_bounds(frame.width, frame.height, x0, y0) && in_bounds(frame.width, frame.height, x1, y1) &&
            bayer_channel(frame.metadata.bayer, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0)) == channel &&
            bayer_channel(frame.metadata.bayer, static_cast<std::uint32_t>(x1), static_cast<std::uint32_t>(y1)) == channel) {
            const float a = raw_value(frame, x0, y0);
            const float b = raw_value(frame, x1, y1);
            gradient = std::fabs(a - b);
            found = true;
            return 0.5f * (a + b);
        }
        found = false;
        gradient = std::numeric_limits<float>::max();
        return 0.0f;
    };

    const PixelChannel site = bayer_channel(frame.metadata.bayer, ux, uy);
    if (channel == PixelChannel::Green && site != PixelChannel::Green) {
        float gh = 0.0f, gv = 0.0f;
        bool hasH = false, hasV = false;
        const float horizontal = directional(1, 0, gh, hasH);
        const float vertical = directional(0, 1, gv, hasV);
        if (hasH && (!hasV || gh <= gv)) return horizontal;
        if (hasV) return vertical;
    }

    if (site == PixelChannel::Green && channel != PixelChannel::Green) {
        float gh = 0.0f, gv = 0.0f;
        bool hasH = false, hasV = false;
        const float horizontal = directional(1, 0, gh, hasH);
        const float vertical = directional(0, 1, gv, hasV);
        if (hasH && (!hasV || gh <= gv)) return horizontal;
        if (hasV) return vertical;
    }

    if (site != PixelChannel::Green && channel != PixelChannel::Green) {
        float sum = 0.0f;
        int count = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (std::abs(dx) + std::abs(dy) != 2) continue;
                const int nx = x + dx;
                const int ny = y + dy;
                if (in_bounds(frame.width, frame.height, nx, ny) && bayer_channel(frame.metadata.bayer, static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny)) == channel) {
                    sum += raw_value(frame, nx, ny);
                    ++count;
                }
            }
        }
        if (count > 0) return sum / static_cast<float>(count);
    }

    float weighted = 0.0f;
    float weights = 0.0f;
    for (int radius = 1; radius <= 3; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (!in_bounds(frame.width, frame.height, nx, ny) || bayer_channel(frame.metadata.bayer, static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny)) != channel) continue;
                const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                const float weight = 1.0f / std::max(1.0f, distance);
                weighted += weight * raw_value(frame, nx, ny);
                weights += weight;
            }
        }
        if (weights > 0.0f) break;
    }
    return weights > 0.0f ? weighted / weights : raw_value(frame, x, y);
}

RGBImage demosaic(const NormalizedFrame& frame) {
    RGBImage image(frame.width, frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            image.at(x, y, PixelChannel::Red) = interpolate_missing(frame, static_cast<int>(x), static_cast<int>(y), PixelChannel::Red);
            image.at(x, y, PixelChannel::Green) = interpolate_missing(frame, static_cast<int>(x), static_cast<int>(y), PixelChannel::Green);
            image.at(x, y, PixelChannel::Blue) = interpolate_missing(frame, static_cast<int>(x), static_cast<int>(y), PixelChannel::Blue);
        }
    }
    return image;
}

void apply_camera_color(RGBImage& image, const TuningProfile& profile) {
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float r = image.at(x, y, PixelChannel::Red);
            const float g = image.at(x, y, PixelChannel::Green);
            const float b = image.at(x, y, PixelChannel::Blue);
            image.at(x, y, PixelChannel::Red) = clamp01(profile.cameraColorMatrix[0] * r + profile.cameraColorMatrix[1] * g + profile.cameraColorMatrix[2] * b);
            image.at(x, y, PixelChannel::Green) = clamp01(profile.cameraColorMatrix[3] * r + profile.cameraColorMatrix[4] * g + profile.cameraColorMatrix[5] * b);
            image.at(x, y, PixelChannel::Blue) = clamp01(profile.cameraColorMatrix[6] * r + profile.cameraColorMatrix[7] * g + profile.cameraColorMatrix[8] * b);
        }
    }
}

void apply_chroma_denoise(RGBImage& image, const TuningProfile& profile, float iso) {
    const float amount = std::max(profile.chromaDenoise, interpolate_iso(profile, iso, &IsoPoint::chromaDenoise));
    if (amount <= 0.0f) return;
    RGBImage copy = image;
    auto luma = [&](const RGBImage& source, int x, int y) {
        if (!in_bounds(source.width, source.height, x, y)) return 0.0f;
        return 0.2126f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Red) +
            0.7152f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Green) +
            0.0722f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Blue);
    };
    for (std::uint32_t y = 1; y + 1 < image.height; ++y) {
        for (std::uint32_t x = 1; x + 1 < image.width; ++x) {
            const float centerLuma = luma(copy, static_cast<int>(x), static_cast<int>(y));
            float uSum = 0.0f, vSum = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const float neighbourLuma = luma(copy, static_cast<int>(x) + dx, static_cast<int>(y) + dy);
                    const float nr = copy.at(static_cast<std::uint32_t>(static_cast<int>(x) + dx), static_cast<std::uint32_t>(static_cast<int>(y) + dy), PixelChannel::Red);
                    const float nb = copy.at(static_cast<std::uint32_t>(static_cast<int>(x) + dx), static_cast<std::uint32_t>(static_cast<int>(y) + dy), PixelChannel::Blue);
                    uSum += nr - neighbourLuma;
                    vSum += nb - neighbourLuma;
                }
            }
            const float avgU = uSum / 9.0f;
            const float avgV = vSum / 9.0f;
            const float edge = std::fabs(luma(copy, static_cast<int>(x) + 1, static_cast<int>(y)) - luma(copy, static_cast<int>(x) - 1, static_cast<int>(y))) +
                std::fabs(luma(copy, static_cast<int>(x), static_cast<int>(y) + 1) - luma(copy, static_cast<int>(x), static_cast<int>(y) - 1));
            const float protection = 1.0f - profile.textureProtection * smoothstep(0.02f, 0.20f, edge);
            const float blend = clamp01(amount * protection);
            const float currentU = copy.at(x, y, PixelChannel::Red) - centerLuma;
            const float currentV = copy.at(x, y, PixelChannel::Blue) - centerLuma;
            image.at(x, y, PixelChannel::Red) = clamp01(centerLuma + currentU * (1.0f - blend) + avgU * blend);
            image.at(x, y, PixelChannel::Blue) = clamp01(centerLuma + currentV * (1.0f - blend) + avgV * blend);
        }
    }
}

// Chroma denoise leaves luma untouched, so whatever grain survives the merge is
// exactly what reads as RAW noise. A bilateral pass whose range scale is the
// post-merge noise model removes that grain in flat areas while the range weight
// keeps real edges. The scale shrinks as sqrt(frames), so a longer burst is not
// over-smoothed into plastic.
void apply_luma_denoise(RGBImage& image, const TuningProfile& profile, float iso, std::uint32_t frameCount) {
    const float amount = profile.lumaDenoise;
    if (amount <= 0.0f || frameCount == 0 || image.width < 5 || image.height < 5) return;
    const float readNoise = interpolate_iso(profile, iso, &IsoPoint::readNoise);
    const float shotCoefficient = interpolate_iso(profile, iso, &IsoPoint::shotCoefficient);
    const float inverseFrames = 1.0f / static_cast<float>(frameCount);
    RGBImage copy = image;
    auto luma = [&](const RGBImage& source, int x, int y) {
        if (!in_bounds(source.width, source.height, x, y)) return 0.0f;
        return 0.2126f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Red) +
            0.7152f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Green) +
            0.0722f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Blue);
    };
    for (std::uint32_t y = 2; y + 2 < image.height; ++y) {
        for (std::uint32_t x = 2; x + 2 < image.width; ++x) {
            const int cx = static_cast<int>(x);
            const int cy = static_cast<int>(y);
            const float center = luma(copy, cx, cy);
            // The merge averages the sensor noise model down by the frame count.
            const float variance = (readNoise * readNoise + shotCoefficient * std::max(0.0f, center)) * inverseFrames;
            const float sigma = std::sqrt(std::max(1e-10f, variance));
            const float rangeScale = std::max(1e-4f, 2.0f * sigma);
            const float inverseRange = 1.0f / (rangeScale * rangeScale);
            float weighted = 0.0f;
            float total = 0.0f;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const float value = luma(copy, cx + dx, cy + dy);
                    const float distance = static_cast<float>(dx * dx + dy * dy);
                    const float difference = value - center;
                    // A rational range kernel keeps this to one divide per tap; an
                    // exp() kernel over a whole frame is not affordable on device.
                    const float weight = (1.0f / (1.0f + 0.25f * distance)) / (1.0f + difference * difference * inverseRange);
                    weighted += weight * value;
                    total += weight;
                }
            }
            if (total <= 0.0f) continue;
            const float edge = std::fabs(luma(copy, cx + 1, cy) - luma(copy, cx - 1, cy)) +
                std::fabs(luma(copy, cx, cy + 1) - luma(copy, cx, cy - 1));
            const float structure = 1.0f - profile.textureProtection * smoothstep(rangeScale * 4.0f, rangeScale * 16.0f, edge);
            // Additive luma correction: it leaves the chroma differences the chroma
            // pass produced exactly where they are and cannot go negative in shadow.
            const float delta = (weighted / total - center) * clamp01(amount * structure);
            image.at(x, y, PixelChannel::Red) = clamp01(image.at(x, y, PixelChannel::Red) + delta);
            image.at(x, y, PixelChannel::Green) = clamp01(image.at(x, y, PixelChannel::Green) + delta);
            image.at(x, y, PixelChannel::Blue) = clamp01(image.at(x, y, PixelChannel::Blue) + delta);
        }
    }
}

void apply_tone_and_detail(RGBImage& image, const TuningProfile& profile) {
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            for (PixelChannel channel : {PixelChannel::Red, PixelChannel::Green, PixelChannel::Blue}) {
                float value = std::max(0.0f, image.at(x, y, channel) - profile.blackPoint);
                value *= std::pow(2.0f, profile.exposure);
                if (value < 0.25f) {
                    value *= 1.0f + profile.shadowCompression * (0.25f - value);
                }
                value = std::pow(clamp01(value), 1.0f / std::max(0.25f, profile.midtoneContrast));
                if (value > 0.62f) {
                    const float t = clamp01((value - 0.62f) / 0.38f);
                    const float roll = 1.0f + profile.highlightRolloff * 3.0f;
                    const float shaped = (1.0f - std::exp(-roll * t)) / std::max(1e-6f, 1.0f - std::exp(-roll));
                    value = 0.62f + 0.38f * ((1.0f - profile.shoulder) * t + profile.shoulder * shaped);
                }
                image.at(x, y, channel) = clamp01(value);
            }
        }
    }

    RGBImage base = image;
    auto luma = [&](const RGBImage& source, int x, int y) {
        if (!in_bounds(source.width, source.height, x, y)) return 0.0f;
        return 0.2126f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Red) +
            0.7152f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Green) +
            0.0722f * source.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), PixelChannel::Blue);
    };
    for (std::uint32_t y = 1; y + 1 < image.height; ++y) {
        for (std::uint32_t x = 1; x + 1 < image.width; ++x) {
            const float center = luma(base, static_cast<int>(x), static_cast<int>(y));
            float local = 0.0f;
            float minimum = 1.0f;
            float maximum = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const float value = luma(base, static_cast<int>(x) + dx, static_cast<int>(y) + dy);
                    local += value;
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                }
            }
            local /= 9.0f;
            const float high = center - local;
            const float edge = std::fabs(luma(base, static_cast<int>(x) + 1, static_cast<int>(y)) - luma(base, static_cast<int>(x) - 1, static_cast<int>(y))) +
                std::fabs(luma(base, static_cast<int>(x), static_cast<int>(y) + 1) - luma(base, static_cast<int>(x), static_cast<int>(y) - 1));
            const float detailConfidence = smoothstep(profile.microcontrastThreshold, 0.25f, std::fabs(high));
            const float edgeConfidence = smoothstep(0.02f, 0.25f, edge);
            const float amount = (profile.localContrast + profile.microcontrastAmount + profile.fineSharpen) *
                (0.35f + 0.65f * detailConfidence) * (0.30f + 0.70f * edgeConfidence);
            for (PixelChannel channel : {PixelChannel::Red, PixelChannel::Green, PixelChannel::Blue}) {
                const float value = base.at(x, y, channel) + high * amount;
                const float allowedLow = std::max(0.0f, minimum - profile.haloProtection * 0.03f);
                const float allowedHigh = std::min(1.0f, maximum + profile.haloProtection * 0.03f);
                image.at(x, y, channel) = std::max(allowedLow, std::min(allowedHigh, value));
            }
        }
    }
}

std::string escape_json(const std::string& value) {
    std::string result;
    for (const char c : value) {
        if (c == '\\' || c == '"') result.push_back('\\');
        result.push_back(c);
    }
    return result;
}

} // namespace

// A frame is normalized as it is packed, so the engine can hold a whole burst at two
// bytes per pixel per frame and release the sensor RAW immediately. The packing must
// use the profile and white balance the merge will run with, so load the tuning profile
// and take `burst_white_balance` from the first frame before adding any frame.
PackedFrame pack_frame(const RawFrame& raw, const TuningProfile& profile, const std::array<float, 3>& burstWb) {
    if (!raw.valid()) throw std::invalid_argument("RAW frame failed validation");
    PackedFrame frame;
    frame.metadata = raw.metadata;
    frame.width = raw.metadata.width;
    frame.height = raw.metadata.height;
    frame.samples.assign(static_cast<std::size_t>(frame.width) * frame.height, 0);

    const float black = raw.metadata.blackLevel;
    const float white = std::max(black + 1.0f, raw.metadata.whiteLevel);
    const float range = white - black;
    // Reused across pixels: keeping this allocation out of the per-pixel loop
    // matters for multi-megapixel bursts on device.
    std::vector<float> neighbours;
    neighbours.reserve(4);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            float sensor = static_cast<float>(raw.at(x, y));

            // Replace only an isolated CFA outlier whose same-colour neighbours
            // agree. A bright object therefore remains untouched when its local
            // same-colour samples are also bright or structurally varied.
            neighbours.clear();
            const PixelChannel channel = bayer_channel(raw.metadata.bayer, x, y);
            for (const auto offset : {std::pair<int, int>{-2, 0}, {2, 0}, {0, -2}, {0, 2}}) {
                const int nx = static_cast<int>(x) + offset.first;
                const int ny = static_cast<int>(y) + offset.second;
                if (in_bounds(frame.width, frame.height, nx, ny) && bayer_channel(raw.metadata.bayer, static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny)) == channel) {
                    neighbours.push_back(static_cast<float>(raw.at(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny))));
                }
            }
            if (neighbours.size() >= 3) {
                std::sort(neighbours.begin(), neighbours.end());
                const float median = neighbours[neighbours.size() / 2U];
                const float spread = neighbours.back() - neighbours.front();
                if (spread < range * 0.08f && std::fabs(sensor - median) > range * 0.18f) {
                    sensor = median;
                }
            }

            float value = clamp01((sensor - black) / range);
            const std::size_t channelIndex = static_cast<std::size_t>(channel);
            value *= burstWb[channelIndex] / std::max(0.01f, burstWb[1]);

            // Flat-field correction is deliberately a bounded calibration model.
            // The default polynomial is identity; profiles may supply measured
            // radial coefficients rather than an invented correction.
            const float nx = (2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(frame.width)) - 1.0f;
            const float ny = (2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(frame.height)) - 1.0f;
            const float radius2 = std::min(1.0f, nx * nx + ny * ny);
            const float radial = profile.lensShadingPolynomial[0] +
                profile.lensShadingPolynomial[1] * radius2 +
                profile.lensShadingPolynomial[2] * radius2 * radius2 +
                profile.lensShadingPolynomial[3] * radius2 * radius2 * radius2;
            value *= std::max(0.25f, std::min(4.0f, radial));
            // The merge only needs about 1e-4 of precision, so the normalized frame is
            // stored in 16 bits instead of two float planes.
            frame.samples[static_cast<std::size_t>(y) * frame.width + x] = static_cast<std::uint16_t>(std::lround(clamp01(value) * kNormalizedScale));
        }
    }

    return frame;
}

const char* bayer_pattern_name(BayerPattern pattern) noexcept {
    switch (pattern) {
        case BayerPattern::RGGB: return "RGGB";
        case BayerPattern::BGGR: return "BGGR";
        case BayerPattern::GRBG: return "GRBG";
        case BayerPattern::GBRG: return "GBRG";
        default: return "Unknown";
    }
}

bool parse_bayer_pattern(const std::string& value, BayerPattern& pattern) noexcept {
    if (value == "RGGB") pattern = BayerPattern::RGGB;
    else if (value == "BGGR") pattern = BayerPattern::BGGR;
    else if (value == "GRBG") pattern = BayerPattern::GRBG;
    else if (value == "GBRG") pattern = BayerPattern::GBRG;
    else { pattern = BayerPattern::Unknown; return false; }
    return true;
}

PixelChannel bayer_channel(BayerPattern pattern, std::uint32_t x, std::uint32_t y) noexcept {
    const bool evenX = (x & 1U) == 0U;
    const bool evenY = (y & 1U) == 0U;
    switch (pattern) {
        case BayerPattern::RGGB:
            return evenY ? (evenX ? PixelChannel::Red : PixelChannel::Green) : (evenX ? PixelChannel::Green : PixelChannel::Blue);
        case BayerPattern::BGGR:
            return evenY ? (evenX ? PixelChannel::Blue : PixelChannel::Green) : (evenX ? PixelChannel::Green : PixelChannel::Red);
        case BayerPattern::GRBG:
            return evenY ? (evenX ? PixelChannel::Green : PixelChannel::Red) : (evenX ? PixelChannel::Blue : PixelChannel::Green);
        case BayerPattern::GBRG:
            return evenY ? (evenX ? PixelChannel::Green : PixelChannel::Blue) : (evenX ? PixelChannel::Red : PixelChannel::Green);
        default:
            return PixelChannel::Green;
    }
}

bool RawFrame::valid() const noexcept {
    const std::size_t minimumStride = static_cast<std::size_t>(metadata.width) * sizeof(std::uint16_t);
    const std::size_t expectedPixels = static_cast<std::size_t>(metadata.width) * metadata.height;
    const std::size_t stride = metadata.rowStrideBytes == 0 ? minimumStride : metadata.rowStrideBytes;
    return metadata.width > 0 && metadata.height > 0 && metadata.bitDepth > 0 && metadata.bitDepth <= 16 &&
        metadata.bayer != BayerPattern::Unknown && metadata.whiteLevel > metadata.blackLevel &&
        stride >= minimumStride && (stride % sizeof(std::uint16_t)) == 0U && pixels.size() >= expectedPixels;
}

std::uint16_t RawFrame::at(std::uint32_t x, std::uint32_t y) const noexcept {
    return pixels[static_cast<std::size_t>(y) * metadata.width + x];
}

std::uint16_t& RawFrame::at(std::uint32_t x, std::uint32_t y) noexcept {
    return pixels[static_cast<std::size_t>(y) * metadata.width + x];
}

std::array<float, 3> burst_white_balance(const RawFrame& reference, const TuningProfile& profile) {
    return usable_white_balance(reference, profile);
}

ProcessResult merge_packed_frames(const std::vector<PackedFrame>& frames, const TuningProfile& profile) {
    if (frames.empty()) throw std::invalid_argument("Cannot process an empty RAW burst");
    if (profile.maxFrames == 0) throw std::invalid_argument("RAW burst profile allows no frames");
    const auto start = std::chrono::steady_clock::now();

    const std::uint32_t width = frames.front().width;
    const std::uint32_t height = frames.front().height;
    const std::size_t frameLimit = std::min<std::size_t>(frames.size(), profile.maxFrames);
    for (std::size_t i = 0; i < frameLimit; ++i) {
        const PackedFrame& frame = frames[i];
        if (frame.width != width || frame.height != height || frame.metadata.bayer != frames.front().metadata.bayer ||
            frame.samples.size() < static_cast<std::size_t>(width) * height) {
            throw std::invalid_argument("RAW burst frames do not share dimensions and Bayer pattern");
        }
    }

    // The burst owns one 16-bit plane per frame, so the merge walks pointers into it
    // instead of copying frames.
    std::vector<const PackedFrame*> normalized;
    normalized.reserve(frameLimit);
    for (std::size_t i = 0; i < frameLimit; ++i) normalized.push_back(&frames[i]);

    std::vector<float> luma;
    std::size_t referenceIndex = 0;
    float bestSharpness = -1.0f;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        luma = packed_luma(*normalized[i]);
        const float sharpness = gradient_energy(luma, width, height);
        if (sharpness > bestSharpness) {
            bestSharpness = sharpness;
            referenceIndex = i;
        }
    }
    if (referenceIndex != 0) std::swap(normalized[0], normalized[referenceIndex]);

    ProcessingDiagnostics diagnostics;
    diagnostics.profileName = profile.name;
    diagnostics.inputFrames = static_cast<std::uint32_t>(normalized.size());
    diagnostics.width = width;
    diagnostics.height = height;
    diagnostics.alignments.resize(normalized.size());
    diagnostics.alignments[0].confidence = 1.0f;
    diagnostics.alignments[0].accepted = true;

    const std::vector<float> referenceLuma = packed_luma(*normalized[0]);
    for (std::size_t i = 1; i < normalized.size(); ++i) {
        luma = packed_luma(*normalized[i]);
        diagnostics.alignments[i] = estimate_alignment(referenceLuma, luma, width, height, profile);
    }
    // The scratch plane is dead once every frame is aligned: release it before the
    // merge allocates its accumulators.
    std::vector<float>().swap(luma);

    std::size_t accepted = 0;
    for (const AlignmentEstimate& alignment : diagnostics.alignments) if (alignment.accepted) ++accepted;
    if (accepted == 0) throw std::runtime_error("No usable frames remained after alignment");
    diagnostics.acceptedFrames = static_cast<std::uint32_t>(accepted);
    diagnostics.rejectedFrames = diagnostics.inputFrames - diagnostics.acceptedFrames;

    NormalizedFrame merged;
    merged.metadata = normalized[0]->metadata;
    merged.width = width;
    merged.height = height;
    merged.linear.assign(static_cast<std::size_t>(width) * height, 0.0f);
    double confidenceSum = 0.0;
    double motionSum = 0.0;
    const float readNoise = interpolate_iso(profile, normalized[0]->metadata.iso, &IsoPoint::readNoise);
    const float shotCoefficient = interpolate_iso(profile, normalized[0]->metadata.iso, &IsoPoint::shotCoefficient);

    std::vector<float> samples;
    std::vector<float> weights;
    samples.reserve(accepted);
    weights.reserve(accepted);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            samples.clear();
            weights.clear();
            float firstEstimate = 0.0f;
            float firstWeight = 0.0f;
            bool hasUnsaturated = false;
            for (std::size_t i = 0; i < normalized.size(); ++i) {
                if (!diagnostics.alignments[i].accepted) continue;
                const AlignmentEstimate& alignment = diagnostics.alignments[i];
                const float value = sample_bilinear(normalized[i]->samples, width, height, static_cast<float>(x) + alignment.dx, static_cast<float>(y) + alignment.dy);
                if (value < 0.995f) hasUnsaturated = true;
                samples.push_back(value);
                const float variance = readNoise * readNoise + shotCoefficient * std::max(0.0f, value);
                float weight = alignment.confidence / std::max(1e-6f, variance);
                // Comparing each frame against the single noisy reference frame with a
                // fixed threshold classifies sensor noise as motion in every textured
                // region, which pins the merge to the reference. The robust reweighting
                // below rejects real outliers against the merged estimate instead.
                if (value > 0.995f && hasUnsaturated) weight = 0.0f;
                weights.push_back(weight);
                firstEstimate += value * weight;
                firstWeight += weight;
            }
            if (samples.empty()) continue;
            const float initial = firstWeight > 0.0f
                ? firstEstimate / firstWeight
                : sample_bilinear(normalized[0]->samples, width, height, static_cast<float>(x), static_cast<float>(y));
            float estimate = initial;
            const int iterations = 2;
            for (int iteration = 0; iteration < iterations; ++iteration) {
                float weighted = 0.0f;
                float total = 0.0f;
                float outliers = 0.0f;
                const bool lastIteration = iteration + 1 == iterations;
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    const float variance = readNoise * readNoise + shotCoefficient * std::max(0.0f, samples[i]);
                    const float robust = robust_weight(std::fabs(samples[i] - estimate), variance, profile);
                    const float weight = weights[i] * robust;
                    weighted += samples[i] * weight;
                    total += weight;
                    // A sample the robust weighting cuts in half disagrees with the merge
                    // far beyond the noise model, so it is the burst's real motion rather
                    // than sensor grain.
                    if (lastIteration && weights[i] > 0.0f && robust < 0.5f) outliers += 1.0;
                }
                if (total > 0.0f) estimate = weighted / total;
                if (lastIteration) motionSum += outliers;
            }
            merged.linear[static_cast<std::size_t>(y) * width + x] = clamp01(estimate);
            confidenceSum += firstWeight > 0.0f ? std::min(1.0f, firstWeight * readNoise * readNoise) : 0.0;
        }
    }
    const double pixelCount = static_cast<double>(width) * height;
    diagnostics.motionFraction = static_cast<float>(motionSum / std::max(1.0, pixelCount * static_cast<double>(accepted)));
    diagnostics.mergeConfidence = static_cast<float>(confidenceSum / std::max(1.0, pixelCount));

    if (profile.enableSubpixelReconstruction && normalized.size() > 1) {
        // Conservative data-fidelity back-projection: each update is made only
        // from measured, aligned samples and is regularized toward the merged
        // estimate. It improves fractional-shift consistency without inventing
        // a higher-frequency pattern unsupported by the burst.
        const int iterations = 2;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            for (std::uint32_t y = 1; y + 1 < height; ++y) {
                for (std::uint32_t x = 1; x + 1 < width; ++x) {
                    float correction = 0.0f;
                    float weightSum = 0.0f;
                    const float current = raw_value(merged, static_cast<int>(x), static_cast<int>(y));
                    for (std::size_t i = 1; i < normalized.size(); ++i) {
                        if (!diagnostics.alignments[i].accepted) continue;
                        const auto& alignment = diagnostics.alignments[i];
                        const float observation = sample_bilinear(normalized[i]->samples, width, height, static_cast<float>(x) + alignment.dx, static_cast<float>(y) + alignment.dy);
                        const float localGradient = std::fabs(raw_value(merged, static_cast<int>(x) + 1, static_cast<int>(y)) - raw_value(merged, static_cast<int>(x) - 1, static_cast<int>(y))) +
                            std::fabs(raw_value(merged, static_cast<int>(x), static_cast<int>(y) + 1) - raw_value(merged, static_cast<int>(x), static_cast<int>(y) - 1));
                        const float weight = alignment.confidence * (1.0f - profile.subpixelRegularization * smoothstep(0.20f, 0.8f, localGradient));
                        correction += weight * (observation - current);
                        weightSum += weight;
                    }
                    if (weightSum > 0.0f) merged.linear[static_cast<std::size_t>(y) * width + x] = clamp01(current + profile.subpixelStrength * correction / weightSum);
                }
            }
        }
    }

    RGBImage output = demosaic(merged);
    apply_camera_color(output, profile);
    apply_luma_denoise(output, profile, merged.metadata.iso, diagnostics.acceptedFrames);
    apply_chroma_denoise(output, profile, merged.metadata.iso);
    apply_tone_and_detail(output, profile);

    const auto end = std::chrono::steady_clock::now();
    diagnostics.processingMilliseconds = std::chrono::duration<double, std::milli>(end - start).count();
    diagnostics.meanAlignmentConfidence = 0.0f;
    diagnostics.meanAlignmentResidual = 0.0f;
    for (const AlignmentEstimate& alignment : diagnostics.alignments) {
        diagnostics.meanAlignmentConfidence += alignment.confidence;
        diagnostics.meanAlignmentResidual += alignment.residualError;
    }
    if (!diagnostics.alignments.empty()) {
        diagnostics.meanAlignmentConfidence /= static_cast<float>(diagnostics.alignments.size());
        diagnostics.meanAlignmentResidual /= static_cast<float>(diagnostics.alignments.size());
    }
    // One packed 16-bit plane per frame, plus the two float luma planes, the merged
    // plane, and the full-frame render copies the denoise and detail stages keep
    // while they read their neighbours. Those stages run in sequence, so the largest
    // of them sets the working set rather than the sum.
    const double packedBytes = static_cast<double>(normalized.size()) * width * height * sizeof(std::uint16_t);
    const double workingBytes = static_cast<double>(width) * height * sizeof(float) * 9.0;
    diagnostics.peakMemoryMiB = static_cast<float>((packedBytes + workingBytes) / (1024.0 * 1024.0));
    return {std::move(output), std::move(diagnostics)};
}

ProcessResult process_burst(const std::vector<RawFrame>& frames, const TuningProfile& profile) {
    if (frames.empty()) throw std::invalid_argument("Cannot process an empty RAW burst");
    if (profile.maxFrames == 0) throw std::invalid_argument("RAW burst profile allows no frames");
    if (!frames.front().valid()) throw std::invalid_argument("RAW burst contains an invalid first frame");
    const std::uint32_t width = frames.front().metadata.width;
    const std::uint32_t height = frames.front().metadata.height;
    const std::size_t frameLimit = std::min<std::size_t>(frames.size(), profile.maxFrames);
    for (std::size_t i = 0; i < frameLimit; ++i) {
        if (!frames[i].valid() || frames[i].metadata.width != width || frames[i].metadata.height != height || frames[i].metadata.bayer != frames.front().metadata.bayer) {
            throw std::invalid_argument("RAW burst frames do not share dimensions and Bayer pattern");
        }
    }

    // In-memory convenience path. The engines pack frames as they arrive instead, so
    // they never hold the sensor RAW for the whole burst.
    const std::array<float, 3> burstWb = burst_white_balance(frames.front(), profile);
    std::vector<PackedFrame> packed;
    packed.reserve(frameLimit);
    for (std::size_t i = 0; i < frameLimit; ++i) packed.push_back(pack_frame(frames[i], profile, burstWb));
    return merge_packed_frames(packed, profile);
}

std::string diagnostics_json(const ProcessingDiagnostics& diagnostics) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6)
        << "{\n"
        << "  \"engineVersion\": \"" << GCAM_ENGINE_VERSION << "\",\n"
        << "  \"profile\": \"" << escape_json(diagnostics.profileName) << "\",\n"
        << "  \"inputFrames\": " << diagnostics.inputFrames << ",\n"
        << "  \"acceptedFrames\": " << diagnostics.acceptedFrames << ",\n"
        << "  \"rejectedFrames\": " << diagnostics.rejectedFrames << ",\n"
        << "  \"dimensions\": {\"width\": " << diagnostics.width << ", \"height\": " << diagnostics.height << "},\n"
        << "  \"meanAlignmentConfidence\": " << diagnostics.meanAlignmentConfidence << ",\n"
        << "  \"meanAlignmentResidual\": " << diagnostics.meanAlignmentResidual << ",\n"
        << "  \"motionFraction\": " << diagnostics.motionFraction << ",\n"
        << "  \"mergeConfidence\": " << diagnostics.mergeConfidence << ",\n"
        << "  \"processingMilliseconds\": " << diagnostics.processingMilliseconds << ",\n"
        << "  \"peakMemoryMiB\": " << diagnostics.peakMemoryMiB << ",\n"
        << "  \"alignments\": [\n";
    for (std::size_t i = 0; i < diagnostics.alignments.size(); ++i) {
        const AlignmentEstimate& a = diagnostics.alignments[i];
        out << "    {\"dx\": " << a.dx << ", \"dy\": " << a.dy
            << ", \"rotation\": " << a.rotationRadians << ", \"scale\": " << a.scale
            << ", \"confidence\": " << a.confidence << ", \"residualError\": " << a.residualError
            << ", \"accepted\": " << (a.accepted ? "true" : "false") << "}"
            << (i + 1U == diagnostics.alignments.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    if (!diagnostics.error.empty()) out << ",\n  \"error\": \"" << escape_json(diagnostics.error) << "\"\n";
    out << "}\n";
    return out.str();
}

void write_ppm(const RGBImage& image, const std::string& path) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() < static_cast<std::size_t>(image.width) * image.height * 3U) {
        throw std::invalid_argument("Cannot write an empty image");
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output image: " + path);
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            unsigned char rgb[3]{};
            for (int c = 0; c < 3; ++c) {
                const float linear = clamp01(image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 3U + static_cast<std::size_t>(c)]);
                const float encoded = linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
                rgb[c] = static_cast<unsigned char>(std::lround(clamp01(encoded) * 255.0f));
            }
            out.write(reinterpret_cast<const char*>(rgb), 3);
        }
    }
}

RGBImage read_ppm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open PPM: " + path);
    std::string magic;
    in >> magic;
    if (magic != "P6") throw std::runtime_error("Only binary P6 PPM is supported: " + path);
    std::uint32_t width = 0, height = 0;
    int maxValue = 0;
    in >> width >> height >> maxValue;
    in.get();
    if (width == 0 || height == 0 || maxValue != 255) throw std::runtime_error("Unsupported PPM dimensions or bit depth");
    RGBImage image(width, height);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(width) * height * 3U);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (in.gcount() != static_cast<std::streamsize>(bytes.size())) throw std::runtime_error("Truncated PPM: " + path);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const float encoded = static_cast<float>(bytes[i]) / 255.0f;
        image.pixels[i] = encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }
    return image;
}

} // namespace gcam
