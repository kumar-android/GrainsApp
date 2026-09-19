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

// Alignment matches sub-pixel shifts, and the luma plane it matches on is not
// shift-invariant: every non-green sensel is filled with the mean of its green
// neighbours, so the plane carries a two-pixel parity pattern that a fractional read
// sees as structure. That pattern biases the match towards whole-pixel shifts - the
// fractional part of a real hand-shake offset is then thrown away and the merge blurs.
// A separable 1-2-1 removes exactly the parity period (its response is zero at half the
// sampling rate) while keeping the structure the matcher needs.
std::vector<float> low_pass_luma(const std::vector<float>& source, std::uint32_t width, std::uint32_t height) {
    std::vector<float> horizontal(source.size(), 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            const float centre = source[row + x];
            const float left = x > 0 ? source[row + x - 1] : centre;
            const float right = x + 1 < width ? source[row + x + 1] : centre;
            horizontal[row + x] = 0.25f * left + 0.5f * centre + 0.25f * right;
        }
    }
    std::vector<float> result(source.size(), 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            const float centre = horizontal[row + x];
            const float up = y > 0 ? horizontal[row - width + x] : centre;
            const float down = y + 1 < height ? horizontal[row + width + x] : centre;
            result[row + x] = 0.25f * up + 0.5f * centre + 0.25f * down;
        }
    }
    return result;
}

// Sample the mosaic of an aligned frame without mixing CFA phases.
//
// Bilinear sampling of a Bayer plane reads neighbours that measure a different
// colour and averages them together, which is not just wrong for colour: it also
// low-passes exactly the sub-pixel information a burst exists to capture. A site's
// own colour instead lives on the half-resolution lattice of equal parity, so
// interpolating there keeps every frame's shifted sampling pattern intact. This is
// the same bilinear merge the HDR+ pipeline performs on Bayer data.
//
// The lattice coordinate of output pixel x is (x >> 1) + dx/2, so the fractional
// part is the same for every pixel of a frame; the arithmetic below is deliberately
// per pixel rather than hoisted, because a prepared sampler struct measured several
// times slower than these four inlined taps.
float sample_mosaic(const PackedFrame& frame, std::uint32_t siteX, std::uint32_t siteY, float dx, float dy) {
    const int parityX = static_cast<int>(siteX & 1U);
    const int parityY = static_cast<int>(siteY & 1U);
    const int lastU = (static_cast<int>(frame.width) - 1 - parityX) / 2;
    const int lastV = (static_cast<int>(frame.height) - 1 - parityY) / 2;
    const float u = static_cast<float>(siteX >> 1) + dx * 0.5f;
    const float v = static_cast<float>(siteY >> 1) + dy * 0.5f;
    const int u0 = std::max(0, std::min(lastU, static_cast<int>(std::floor(u))));
    const int v0 = std::max(0, std::min(lastV, static_cast<int>(std::floor(v))));
    const int u1 = std::max(0, std::min(lastU, u0 + 1));
    const int v1 = std::max(0, std::min(lastV, v0 + 1));
    const float tu = std::max(0.0f, std::min(1.0f, u - static_cast<float>(u0)));
    const float tv = std::max(0.0f, std::min(1.0f, v - static_cast<float>(v0)));
    const std::uint32_t left = static_cast<std::uint32_t>(parityX + 2 * u0);
    const std::uint32_t right = static_cast<std::uint32_t>(parityX + 2 * u1);
    const std::uint32_t top = static_cast<std::uint32_t>(parityY + 2 * v0);
    const std::uint32_t bottom = static_cast<std::uint32_t>(parityY + 2 * v1);
    const float upper = packed_value(frame, static_cast<int>(left), static_cast<int>(top)) * (1.0f - tu) +
        packed_value(frame, static_cast<int>(right), static_cast<int>(top)) * tu;
    const float lower = packed_value(frame, static_cast<int>(left), static_cast<int>(bottom)) * (1.0f - tu) +
        packed_value(frame, static_cast<int>(right), static_cast<int>(bottom)) * tu;
    return upper * (1.0f - tv) + lower * tv;
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


// Light a frame recorded, in units that compare across a burst. Exposure time and gain
// multiply into the photons a sensel collected, so the ratio of two frames' products is
// the gain that puts their normalized samples on one scale. A burst the camera re-metered
// while it ran - which is what happens between shots unless exposure is locked - otherwise
// merges frames of different brightness into a ghosted average with the wrong noise model.
float frame_exposure(const RawMetadata& metadata) {
    const float exposure = metadata.iso * metadata.exposureTimeSeconds;
    if (!(exposure > 0.0f) || !std::isfinite(exposure)) return 0.0f;
    return exposure;
}

// Share of a frame's sensels sitting at its own white level. The merge's primary frame is
// chosen with this: a frame whose highlights are blown holds no highlight data, and making
// it the primary pins the merged white to its clipping.
float packed_clipped_fraction(const PackedFrame& frame) {
    if (frame.samples.empty()) return 0.0f;
    const std::uint16_t white = static_cast<std::uint16_t>(0.995f * kNormalizedScale);
    std::size_t clipped = 0;
    for (const std::uint16_t sample : frame.samples) {
        if (sample >= white) ++clipped;
    }
    return static_cast<float>(clipped) / static_cast<float>(frame.samples.size());
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

// A frame is matched by gathering the reference samples once and then reading the
// candidate directly at each candidate displacement: integer offsets never leave the
// frame, so the comparison needs no interpolation and the gather is the only cost.
struct MatchGrid {
    std::vector<int> x;
    std::vector<int> y;
    std::vector<float> value;
};

MatchGrid build_match_grid(const std::vector<float>& reference, std::uint32_t width, std::uint32_t height, int stride, int margin) {
    MatchGrid grid;
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    if (stride < 1 || margin < 1 || w <= 2 * margin || h <= 2 * margin) return grid;
    grid.x.reserve(static_cast<std::size_t>(w / stride + 1) * static_cast<std::size_t>(h / stride + 1));
    for (int y = margin; y + margin < h; y += stride) {
        for (int x = margin; x + margin < w; x += stride) {
            grid.x.push_back(x);
            grid.y.push_back(y);
            grid.value.push_back(sample_bilinear(reference, width, height, static_cast<float>(x), static_cast<float>(y)));
        }
    }
    return grid;
}

float match_error(const std::vector<float>& candidate, std::uint32_t width, const MatchGrid& grid, int dx, int dy) {
    if (grid.value.empty()) return 1.0f;
    double error = 0.0;
    for (std::size_t i = 0; i < grid.value.size(); ++i) {
        const std::size_t index = static_cast<std::size_t>(grid.y[i] + dy) * width + static_cast<std::size_t>(grid.x[i] + dx);
        const float difference = candidate[index] - grid.value[i];
        error += static_cast<double>(difference) * difference;
    }
    return static_cast<float>(error / static_cast<double>(grid.value.size()));
}

// Halve a plane with a 2x2 mean. The result is both the coarse matching plane and, because
// it averages a whole sampling period, a plane free of the two-pixel parity pattern the
// alignment luma carries.
std::vector<float> downsample_halve(const std::vector<float>& source, std::uint32_t width, std::uint32_t height, std::uint32_t& coarseWidth, std::uint32_t& coarseHeight) {
    coarseWidth = std::max(1U, (width + 1U) / 2U);
    coarseHeight = std::max(1U, (height + 1U) / 2U);
    std::vector<float> result(static_cast<std::size_t>(coarseWidth) * coarseHeight, 0.0f);
    for (std::uint32_t y = 0; y < coarseHeight; ++y) {
        for (std::uint32_t x = 0; x < coarseWidth; ++x) {
            float sum = 0.0f;
            int count = 0;
            for (std::uint32_t oy = 0; oy < 2U; ++oy) {
                for (std::uint32_t ox = 0; ox < 2U; ++ox) {
                    const std::uint32_t sx = 2U * x + ox;
                    const std::uint32_t sy = 2U * y + oy;
                    if (sx < width && sy < height) {
                        sum += source[static_cast<std::size_t>(sy) * width + sx];
                        ++count;
                    }
                }
            }
            result[static_cast<std::size_t>(y) * coarseWidth + x] = count > 0 ? sum / static_cast<float>(count) : 0.0f;
        }
    }
    return result;
}

AlignmentEstimate estimate_alignment(const std::vector<float>& referenceLuma, const std::vector<float>& candidateLuma, std::uint32_t width, std::uint32_t height, const TuningProfile& profile) {
    AlignmentEstimate best;
    // Coarse pass first. A burst a third of a second long can drift far enough that the
    // frames at either end sit more than ten pixels apart, and a full-resolution search
    // wide enough to cover that would cost more than the merge it feeds. The old matcher
    // searched a fixed six pixels, so on a long burst every frame past that radius pinned
    // at the limit, stayed misaligned and smeared the merge - the longer the burst, the
    // worse. Matching on a half-resolution plane reaches twice as far for a quarter of the
    // samples, and the full-resolution pass then only corrects what the coarse pass found.
    // A burst the camera re-metered between shots records the same scene at a different gain,
    // and a bracket records it at different gains on purpose. The match cost below is an
    // unnormalized sum of squared differences, so two exposures of one scene disagree by far
    // more than any misalignment: matched against a brighter reference, a darker frame's
    // residual is dominated by the exposure difference at every displacement, so its minimum is
    // set by wherever the reference happens to be dimmest rather than by where the frames line
    // up, and the merge then samples the wrong sensel with a confident-looking residual.
    // Rescaling the
    // candidate to the reference by the least-squares gain through the origin removes exactly
    // that exposure ratio and leaves every threshold below with the meaning it has for a burst
    // recorded at one exposure.
    double referenceDotCandidate = 0.0;
    double candidateEnergy = 0.0;
    const std::size_t compared = std::min(referenceLuma.size(), candidateLuma.size());
    for (std::size_t i = 0; i < compared; ++i) {
        referenceDotCandidate += static_cast<double>(referenceLuma[i]) * static_cast<double>(candidateLuma[i]);
        candidateEnergy += static_cast<double>(candidateLuma[i]) * static_cast<double>(candidateLuma[i]);
    }
    std::vector<float> exposureMatched;
    if (candidateEnergy > 1e-9) {
        const float gain = static_cast<float>(referenceDotCandidate / candidateEnergy);
        if (std::fabs(gain - 1.0f) > 1e-3f) {
            const float bounded = std::max(1.0f / 64.0f, std::min(64.0f, gain));
            exposureMatched = candidateLuma;
            for (float& value : exposureMatched) value *= bounded;
        }
    }
    const std::vector<float>& candidate = exposureMatched.empty() ? candidateLuma : exposureMatched;

    std::uint32_t coarseWidth = 0;
    std::uint32_t coarseHeight = 0;
    const std::vector<float> referenceCoarse = downsample_halve(referenceLuma, width, height, coarseWidth, coarseHeight);
    const std::vector<float> candidateCoarse = downsample_halve(candidate, width, height, coarseWidth, coarseHeight);
    const int coarseLimit = std::max(0, static_cast<int>(std::min(coarseWidth, coarseHeight) / 2U) - 2);
    const int coarseRadius = std::min(12, coarseLimit);
    const int coarseStride = coarseWidth > 128U ? 4 : 2;
    const MatchGrid coarseGrid = build_match_grid(referenceCoarse, coarseWidth, coarseHeight, coarseStride, coarseRadius + 1);
    int coarseDx = 0;
    int coarseDy = 0;
    float coarseError = std::numeric_limits<float>::max();
    for (int dy = -coarseRadius; dy <= coarseRadius; ++dy) {
        for (int dx = -coarseRadius; dx <= coarseRadius; ++dx) {
            const float error = match_error(candidateCoarse, coarseWidth, coarseGrid, dx, dy);
            if (error < coarseError) {
                coarseError = error;
                coarseDx = dx;
                coarseDy = dy;
            }
        }
    }
    // Full-resolution pass. The coarse displacement is in half-resolution pixels and is
    // only good to one of them, so this searches a small window around twice it.
    const int fineRadius = 2;
    const int fineStride = width > 256U ? 4 : 2;
    int fineMargin = 2 * coarseRadius + fineRadius + 1;
    fineMargin = std::min(fineMargin, std::max(1, static_cast<int>(std::min(width, height) / 2U) - 2));
    const MatchGrid fineGrid = build_match_grid(referenceLuma, width, height, fineStride, fineMargin);
    const int anchorLimit = std::max(0, fineMargin - fineRadius);
    const int anchorX = std::max(-anchorLimit, std::min(anchorLimit, coarseDx * 2));
    const int anchorY = std::max(-anchorLimit, std::min(anchorLimit, coarseDy * 2));
    int integerX = anchorX;
    int integerY = anchorY;
    float bestError = std::numeric_limits<float>::max();
    for (int dy = anchorY - fineRadius; dy <= anchorY + fineRadius; ++dy) {
        for (int dx = anchorX - fineRadius; dx <= anchorX + fineRadius; ++dx) {
            const float error = match_error(candidate, width, fineGrid, dx, dy);
            if (error < bestError) {
                bestError = error;
                integerX = dx;
                integerY = dy;
            }
        }
    }
    // Sub-pixel refinement by the standard parabolic fit: the error at the integer minimum
    // and at its four axial neighbours gives one parabola per axis, and its peak is how far
    // the true minimum lies from the grid. A search over candidate fractions instead picks
    // whichever candidate the noise happened to favour, which warps frames by up to half a
    // pixel and blurs the merge exactly where the burst should gain resolution. Fitting the
    // curvature also makes the correction self-limiting: a flat error surface yields zero.
    auto errorAtOffset = [&](float dx, float dy) {
        if (fineGrid.value.empty()) return 1.0f;
        double error = 0.0;
        for (std::size_t i = 0; i < fineGrid.value.size(); ++i) {
            const float difference = sample_bilinear(candidate, width, height, static_cast<float>(fineGrid.x[i]) + dx, static_cast<float>(fineGrid.y[i]) + dy) - fineGrid.value[i];
            error += static_cast<double>(difference) * difference;
        }
        return static_cast<float>(error / static_cast<double>(fineGrid.value.size()));
    };
    const float errorPositiveX = errorAtOffset(static_cast<float>(integerX + 1), static_cast<float>(integerY));
    const float errorNegativeX = errorAtOffset(static_cast<float>(integerX - 1), static_cast<float>(integerY));
    const float errorPositiveY = errorAtOffset(static_cast<float>(integerX), static_cast<float>(integerY + 1));
    const float errorNegativeY = errorAtOffset(static_cast<float>(integerX), static_cast<float>(integerY - 1));
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    const float curvatureX = errorPositiveX + errorNegativeX - 2.0f * bestError;
    if (curvatureX > 1e-12f) offsetX = std::max(-0.5f, std::min(0.5f, 0.5f * (errorNegativeX - errorPositiveX) / curvatureX));
    const float curvatureY = errorPositiveY + errorNegativeY - 2.0f * bestError;
    if (curvatureY > 1e-12f) offsetY = std::max(-0.5f, std::min(0.5f, 0.5f * (errorNegativeY - errorPositiveY) / curvatureY));
    best.dx = static_cast<float>(integerX) + offsetX;
    best.dy = static_cast<float>(integerY) + offsetY;
    const float refinedError = (offsetX == 0.0f && offsetY == 0.0f) ? bestError : errorAtOffset(best.dx, best.dy);
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


RGBImage demosaic(const NormalizedFrame& frame) {
    const std::uint32_t width = frame.width;
    const std::uint32_t height = frame.height;
    RGBImage image(width, height);
    // Pass one: a green sample at every sensel. Hamilton-Adams interpolates along
    // the direction with the smaller gradient and corrects with the centre
    // channel's own second derivative, so a ramp is reproduced exactly instead of
    // being averaged away. Green carries most of the luminance detail.
    std::vector<float> green(static_cast<std::size_t>(width) * height, 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (bayer_channel(frame.metadata.bayer, x, y) == PixelChannel::Green) {
                green[index] = raw_value(frame, static_cast<int>(x), static_cast<int>(y));
                continue;
            }
            const int cx = static_cast<int>(x);
            const int cy = static_cast<int>(y);
            const float centre = raw_value(frame, cx, cy);
            bool hasH = false, hasV = false;
            float estimateH = 0.0f, estimateV = 0.0f, costH = 0.0f, costV = 0.0f;
            auto directional_green = [&](int dx, int dy, float& estimate, float& cost, bool& found) {
                if (!in_bounds(width, height, cx - dx, cy - dy) || !in_bounds(width, height, cx + dx, cy + dy)) return;
                const float a = raw_value(frame, cx - dx, cy - dy);
                const float b = raw_value(frame, cx + dx, cy + dy);
                estimate = 0.5f * (a + b);
                cost = std::fabs(a - b);
                if (in_bounds(width, height, cx - 2 * dx, cy - 2 * dy) && in_bounds(width, height, cx + 2 * dx, cy + 2 * dy)) {
                    const float a2 = raw_value(frame, cx - 2 * dx, cy - 2 * dy);
                    const float b2 = raw_value(frame, cx + 2 * dx, cy + 2 * dy);
                    const float correction = 0.25f * (2.0f * centre - a2 - b2);
                    estimate += correction;
                    cost += std::fabs(correction) * 4.0f;
                }
                found = true;
            };
            directional_green(1, 0, estimateH, costH, hasH);
            directional_green(0, 1, estimateV, costV, hasV);
            if (hasH && (!hasV || costH <= costV)) green[index] = estimateH;
            else if (hasV) green[index] = estimateV;
            else green[index] = centre;
        }
    }
    // Pass two: chroma by constant colour difference against the green plane.
    // R - G and B - G vary far more slowly than the channels themselves, so this
    // keeps edges that an independent average of the sparser red or blue samples
    // smears across a whole 2x2 cell.
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            image.at(x, y, PixelChannel::Green) = green[index];
            const PixelChannel site = bayer_channel(frame.metadata.bayer, x, y);
            const float centre = raw_value(frame, static_cast<int>(x), static_cast<int>(y));
            for (PixelChannel channel : {PixelChannel::Red, PixelChannel::Blue}) {
                if (site == channel) { image.at(x, y, channel) = centre; continue; }
                float difference = 0.0f;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = static_cast<int>(x) + dx;
                        const int ny = static_cast<int>(y) + dy;
                        if (!in_bounds(width, height, nx, ny)) continue;
                        if (bayer_channel(frame.metadata.bayer, static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny)) != channel) continue;
                        difference += raw_value(frame, nx, ny) - green[static_cast<std::size_t>(ny) * width + static_cast<std::uint32_t>(nx)];
                        ++count;
                    }
                }
                image.at(x, y, channel) = count > 0 ? green[index] + difference / static_cast<float>(count) : centre;
            }
        }
    }
    return image;
}


// The camera matrix is scene-referred: it runs before the display transform, on values that
// white balance has already pushed past one in the brighter colour channels. Clamping here
// would put back exactly the clipping the merge worked to avoid, so only the lower bound is
// enforced and the tone map is left to decide what reaches white.
void apply_camera_color(RGBImage& image, const TuningProfile& profile) {
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float r = image.at(x, y, PixelChannel::Red);
            const float g = image.at(x, y, PixelChannel::Green);
            const float b = image.at(x, y, PixelChannel::Blue);
            image.at(x, y, PixelChannel::Red) = std::max(0.0f, profile.cameraColorMatrix[0] * r + profile.cameraColorMatrix[1] * g + profile.cameraColorMatrix[2] * b);
            image.at(x, y, PixelChannel::Green) = std::max(0.0f, profile.cameraColorMatrix[3] * r + profile.cameraColorMatrix[4] * g + profile.cameraColorMatrix[5] * b);
            image.at(x, y, PixelChannel::Blue) = std::max(0.0f, profile.cameraColorMatrix[6] * r + profile.cameraColorMatrix[7] * g + profile.cameraColorMatrix[8] * b);
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
            image.at(x, y, PixelChannel::Red) = std::max(0.0f, centerLuma + currentU * (1.0f - blend) + avgU * blend);
            image.at(x, y, PixelChannel::Blue) = std::max(0.0f, centerLuma + currentV * (1.0f - blend) + avgV * blend);
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
            float spatialWeighted = 0.0f;
            float spatialTotal = 0.0f;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const float value = luma(copy, cx + dx, cy + dy);
                    const float distance = static_cast<float>(dx * dx + dy * dy);
                    const float spatial = 1.0f / (1.0f + 0.25f * distance);
                    const float difference = value - center;
                    // A rational range kernel keeps this to one divide per tap; an
                    // exp() kernel over a whole frame is not affordable on device.
                    const float weight = spatial / (1.0f + difference * difference * inverseRange);
                    weighted += weight * value;
                    total += weight;
                    spatialWeighted += spatial * value;
                    spatialTotal += spatial;
                }
            }
            if (total <= 0.0f || spatialTotal <= 0.0f) continue;
            // How much of the centre is structure rather than its own neighbourhood. A
            // first difference is zero at the crest of a fine pattern, so gating on it
            // alone lets a bilateral pull every peak towards the local mean - which is
            // exactly how a denoise stage eats the finest detail a burst recovered.
            // The centre against its own spatial mean is largest at a crest, so the two
            // measures are combined: either a gradient or a crest protects the pixel.
            const float crest = std::fabs(center - spatialWeighted / spatialTotal);
            const float edge = std::fabs(luma(copy, cx + 1, cy) - luma(copy, cx - 1, cy)) +
                std::fabs(luma(copy, cx, cy + 1) - luma(copy, cx, cy - 1));
            const float activation = std::max(edge, 2.0f * crest);
            // The thresholds are multiples of the post-merge noise, so a longer burst
            // protects less: what counts as structure when the noise floor has dropped
            // by sqrt(frames) is a smaller coefficient.
            const float structure = 1.0f - profile.textureProtection * smoothstep(2.0f * sigma, 8.0f * sigma, activation);
            // Additive luma correction: it leaves the chroma differences the chroma
            // pass produced exactly where they are and cannot go negative in shadow.
            const float delta = (weighted / total - center) * clamp01(amount * structure);
            image.at(x, y, PixelChannel::Red) = std::max(0.0f, image.at(x, y, PixelChannel::Red) + delta);
            image.at(x, y, PixelChannel::Green) = std::max(0.0f, image.at(x, y, PixelChannel::Green) + delta);
            image.at(x, y, PixelChannel::Blue) = std::max(0.0f, image.at(x, y, PixelChannel::Blue) + delta);
        }
    }
}

// Separable box blur with a running sum: O(1) per pixel per axis. Three passes
// approximate a Gaussian at roughly 1.5x the radius, which is the scale the tone
// map needs for a base layer without a cost the GPU path could not match.
std::vector<float> box_blur(const std::vector<float>& source, std::uint32_t width, std::uint32_t height, int radius) {
    if (radius < 1 || width == 0 || height == 0) return source;
    const float inverse = 1.0f / static_cast<float>(2 * radius + 1);
    const int lastX = static_cast<int>(width) - 1;
    const int lastY = static_cast<int>(height) - 1;
    std::vector<float> horizontal(source.size(), 0.0f);
    std::vector<float> output(source.size(), 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        double sum = 0.0;
        for (int i = -radius; i <= radius; ++i) sum += source[row + static_cast<std::size_t>(std::max(0, std::min(lastX, i)))];
        for (std::uint32_t x = 0; x < width; ++x) {
            horizontal[row + x] = static_cast<float>(sum) * inverse;
            sum += source[row + static_cast<std::size_t>(std::min(lastX, static_cast<int>(x) + radius + 1))];
            sum -= source[row + static_cast<std::size_t>(std::max(0, static_cast<int>(x) - radius))];
        }
    }
    for (std::uint32_t x = 0; x < width; ++x) {
        double sum = 0.0;
        for (int i = -radius; i <= radius; ++i) sum += horizontal[static_cast<std::size_t>(std::max(0, std::min(lastY, i))) * width + x];
        for (std::uint32_t y = 0; y < height; ++y) {
            output[static_cast<std::size_t>(y) * width + x] = static_cast<float>(sum) * inverse;
            sum += horizontal[static_cast<std::size_t>(std::min(lastY, static_cast<int>(y) + radius + 1)) * width + x];
            sum -= horizontal[static_cast<std::size_t>(std::max(0, static_cast<int>(y) - radius)) * width + x];
        }
    }
    return output;
}

std::vector<float> blur_plane(const std::vector<float>& source, std::uint32_t width, std::uint32_t height, int radius) {
    std::vector<float> plane = source;
    for (int pass = 0; pass < 3; ++pass) plane = box_blur(plane, width, height, radius);
    return plane;
}

// Local tone mapping: the part that makes a single-exposure burst look like it has
// far more dynamic range than it does. Luminance is split into a smooth base layer
// and a detail layer; the base contrast is compressed toward the scene mean so
// shadows open and highlights stop short of clipping, while the detail layer is
// boosted back. Near a strong edge the base follows a finer scale so the compression
// cannot ring across it, and colour is pulled toward white as a channel approaches
// saturation so a recovered highlight reads as light rather than as a colour cast.
void apply_local_tone_map(RGBImage& image, const TuningProfile& profile, float& toneStops) {
    toneStops = 0.0f;
    const std::uint32_t width = image.width;
    const std::uint32_t height = image.height;
    const float amount = clamp01(profile.localToneStrength);
    if (width < 8 || height < 8 || amount <= 0.0f) return;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    std::vector<float> logLuma(count, 0.0f);
    double total = 0.0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const float luma = std::max(0.0f, 0.2126f * image.at(x, y, PixelChannel::Red) + 0.7152f * image.at(x, y, PixelChannel::Green) + 0.0722f * image.at(x, y, PixelChannel::Blue));
            logLuma[index] = std::log(1e-4f + luma);
            total += logLuma[index];
        }
    }
    const float meanLog = static_cast<float>(total / static_cast<double>(count));
    const int shortSide = static_cast<int>(std::min(width, height));
    const int fineRadius = std::max(1, shortSide / 240);
    const int coarseRadius = std::max(fineRadius + 1, shortSide / 48);
    const std::vector<float> fine = blur_plane(logLuma, width, height, fineRadius);
    const std::vector<float> coarse = blur_plane(logLuma, width, height, coarseRadius);
    // Compression knee in log-luminance: how far from the scene mean the base
    // keeps moving. Past the knee the base stops, which is what leaves shadow and
    // highlight headroom for the global curve instead of clipping.
    const float range = std::max(0.10f, profile.localToneRange);
    const float detailBoost = profile.localContrast * 6.0f;
    const float follow = clamp01(profile.haloProtection);
    const float recovery = clamp01(profile.highlightRecovery);
    const float knee = 0.86f;
    double shiftSum = 0.0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const float logValue = logLuma[index];
            const float gradient = std::fabs(sample_bilinear(logLuma, width, height, static_cast<float>(x) + 1.0f, static_cast<float>(y)) - sample_bilinear(logLuma, width, height, static_cast<float>(x) - 1.0f, static_cast<float>(y))) +
                std::fabs(sample_bilinear(logLuma, width, height, static_cast<float>(x), static_cast<float>(y) + 1.0f) - sample_bilinear(logLuma, width, height, static_cast<float>(x), static_cast<float>(y) - 1.0f));
            const float edgeScale = follow * smoothstep(0.05f, 0.35f, gradient);
            const float base = coarse[index] + (fine[index] - coarse[index]) * edgeScale;
            const float detail = logValue - base;
            const float offset = base - meanLog;
            const float shaped = range * std::tanh(offset / range);
            const float baseShift = (shaped - offset) * amount;
            // A detail layer below the threshold is grain that survived the render
            // denoise, so it is boosted only partially; real texture gets the full gain.
            const float guard = 0.45f + 0.55f * smoothstep(0.003f, 0.025f, std::fabs(detail));
            const float detailShift = detail * detailBoost * guard * amount;
            const float shift = baseShift + detailShift;
            shiftSum += std::fabs(shift);
            const float luma = std::exp(logValue) - 1e-4f;
            const float lifted = std::max(0.0f, std::exp(logValue + shift) - 1e-4f);
            const float ratio = luma > 1e-5f ? lifted / luma : 1.0f;
            float red = image.at(x, y, PixelChannel::Red) * ratio;
            float green = image.at(x, y, PixelChannel::Green) * ratio;
            float blue = image.at(x, y, PixelChannel::Blue) * ratio;
            // The channel levels here are still scene-referred, so how near white a pixel is
            // has to be judged on the level the display will actually show, not on the
            // number before the curve.
            const float peak = std::max(red, std::max(green, blue));
            const float displayedPeak = std::min(1.0f, peak);
            if (displayedPeak > knee && recovery > 0.0f) {
                const float blend = clamp01((displayedPeak - knee) / (1.0f - knee)) * recovery;
                const float lumaNow = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
                red += (lumaNow - red) * blend;
                green += (lumaNow - green) * blend;
                blue += (lumaNow - blue) * blend;
            }
            image.at(x, y, PixelChannel::Red) = clamp01(red);
            image.at(x, y, PixelChannel::Green) = clamp01(green);
            image.at(x, y, PixelChannel::Blue) = clamp01(blue);
        }
    }
    toneStops = static_cast<float>(shiftSum / static_cast<double>(count)) / std::log(2.0f);
}

// A burst that carried range above the sensor's white level holds highlights brighter than
// the display can show at the exposure the camera metered, and every stage below this one
// clamps a channel at one: rendered as it stands, everything the merge recovered above white
// is turned straight back into flat white and the bracket's extra range is thrown away.
// This maps the plane's own white point to white first, with the extended Reinhard shoulder
// v * (1 + v / W^2) / (1 + v). That curve fixes zero, maps W to exactly one, and stays within a
// few hundredths of the identity well below the white point, so the midtones keep the exposure
// the burst was metered at while the recovered range is spread out below white instead of being
// cut off. The white point is a high percentile of the brightest channel rather than the
// brightest pixel, so one specular sensel cannot re-expose the whole frame.
float apply_highlight_shoulder(RGBImage& image) {
    const std::size_t count = image.pixels.size() / 3U;
    if (count == 0) return 1.0f;
    float peak = 0.0f;
    for (const float value : image.pixels) peak = std::max(peak, value);
    if (!(peak > 0.0f)) return 1.0f;

    constexpr int kBins = 1024;
    constexpr std::size_t kPercentileDenominator = 200U;
    std::vector<std::uint32_t> histogram(static_cast<std::size_t>(kBins), 0U);
    for (std::size_t i = 0; i < count; ++i) {
        const float value = std::max(image.pixels[3U * i], std::max(image.pixels[3U * i + 1U], image.pixels[3U * i + 2U]));
        if (!(value > 0.0f)) continue;
        const int bin = std::min(kBins - 1, static_cast<int>(value / peak * static_cast<float>(kBins)));
        histogram[static_cast<std::size_t>(bin)] += 1U;
    }
    // The top half a percent of the frame sits above the white point: the walk accumulates
    // from the brightest bin downwards and stops as soon as it has passed that many pixels.
    const std::size_t wanted = std::max<std::size_t>(1U, count / kPercentileDenominator);
    std::size_t running = 0;
    float white = peak;
    for (int bin = kBins - 1; bin >= 0; --bin) {
        running += histogram[static_cast<std::size_t>(bin)];
        if (running >= wanted) {
            white = peak * (static_cast<float>(bin) + 0.5f) / static_cast<float>(kBins);
            break;
        }
    }
    if (!(white > 1.0f)) return 1.0f;

    // The white point lands just below white rather than exactly on it: a curve that ended at
    // one would leave the brightest pixel of the frame pinned there, which is the clamp this
    // exists to avoid. The margin is the headroom a display curve normally keeps at the top.
    constexpr float kWhiteTarget = 0.95f;
    const float inverseWhiteSquared = 1.0f / (white * white);
    for (float& value : image.pixels) {
        if (value <= 0.0f) continue;
        value = kWhiteTarget * value * (1.0f + value * inverseWhiteSquared) / (1.0f + value);
    }
    return white;
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
            // `localContrast` belongs to the multi-scale tone map's detail layer; this
            // pass is the fine sharpen and keeps only its own two terms.
            const float amount = (profile.microcontrastAmount + profile.fineSharpen) *
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
// bytes per pixel per frame and release the sensor RAW immediately. Packing needs the
// profile, and the white balance it was given is carried on the frame so the merge can
// apply the gains once, after it has combined the burst.
PackedFrame pack_frame(const RawFrame& raw, const TuningProfile& profile, const std::array<float, 3>& burstWb) {
    if (!raw.valid()) throw std::invalid_argument("RAW frame failed validation");
    PackedFrame frame;
    frame.metadata = raw.metadata;
    frame.width = raw.metadata.width;
    frame.height = raw.metadata.height;
    frame.whiteBalance = {
        std::max(0.01f, burstWb[0]) / std::max(0.01f, burstWb[1]),
        std::max(0.01f, burstWb[1]) / std::max(0.01f, burstWb[1]),
        std::max(0.01f, burstWb[2]) / std::max(0.01f, burstWb[1])
    };
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
    float bestScore = -1.0f;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        luma = packed_luma(*normalized[i]);
        const float sharpness = gradient_energy(luma, width, height);
        // Sharpness alone picks the frame that lost the most highlight data, because
        // clipping a highlight raises its local gradient. Discounting a frame by how much
        // of it clipped keeps a blown frame from becoming the primary while still
        // preferring the sharpest frame that kept its highlights.
        const float score = sharpness * (1.0f - smoothstep(0.0025f, 0.04f, packed_clipped_fraction(*normalized[i])));
        if (score > bestScore) {
            bestScore = score;
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


    // Alignment matches on the low-passed plane: it is the shift-invariant one.
    const std::vector<float> referenceLuma = low_pass_luma(packed_luma(*normalized[0]), width, height);
    for (std::size_t i = 1; i < normalized.size(); ++i) {
        luma = packed_luma(*normalized[i]);
        diagnostics.alignments[i] = estimate_alignment(referenceLuma, low_pass_luma(luma, width, height), width, height, profile);
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

    // Only the frames the alignment kept take part in the merge, and each carries its own
    // ISO. A burst that ramps sensitivity mid-capture then weights every frame by the noise
    // that frame actually has instead of by the reference frame's model.
    // Every frame is merged into one exposure so its samples are comparable. The anchor is
    // the brightest exposure the burst recorded: that is the exposure the camera metered,
    // so anchoring there keeps the image's overall level where the camera put it, and a
    // frame the camera underexposed scales up instead of dragging the whole merge down.
    // Where the anchor's white level clipped but a darker frame still resolved the scene,
    // the merge returns a sample above one - the burst's real extra highlight range.
    struct MergeFrame {
        const PackedFrame* frame;
        const AlignmentEstimate* alignment;
        float readNoise;
        float shotCoefficient;
        float scale;
    };
    // The anchor is taken over the whole burst and not over the frames the alignment kept:
    // every scale below derives from it, so a frame the merge rejects must not be able to move
    // the exposure of the frames it does not touch.
    float anchorExposure = 0.0f;
    float smallestExposure = 0.0f;
    for (const PackedFrame* frame : normalized) {
        const float exposure = frame_exposure(frame->metadata);
        if (exposure > 0.0f) {
            anchorExposure = std::max(anchorExposure, exposure);
            smallestExposure = smallestExposure <= 0.0f ? exposure : std::min(smallestExposure, exposure);
        }
    }
    const auto exposure_scale = [anchorExposure](const PackedFrame& frame) {
        const float exposure = frame_exposure(frame.metadata);
        if (!(anchorExposure > 0.0f) || !(exposure > 0.0f)) return 1.0f;
        return std::max(1.0f / 16.0f, std::min(16.0f, anchorExposure / exposure));
    };
    std::vector<MergeFrame> mergeFrames;
    mergeFrames.reserve(normalized.size());
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (!diagnostics.alignments[i].accepted) continue;
        mergeFrames.push_back({normalized[i], &diagnostics.alignments[i],
            interpolate_iso(profile, normalized[i]->metadata.iso, &IsoPoint::readNoise),
            interpolate_iso(profile, normalized[i]->metadata.iso, &IsoPoint::shotCoefficient),
            exposure_scale(*normalized[i])});
    }
    // The primary frame is merged in the anchor's exposure too, so the detail recovery that
    // follows can compare it against the merged plane without a gain between them. Its scale
    // is read from the primary itself rather than from the merge list, which can start with a
    // different frame when the alignment rejects the primary.
    const float primaryScale = exposure_scale(*normalized[0]);
    if (anchorExposure > 0.0f && smallestExposure > 0.0f) {
        diagnostics.exposureRangeStops = static_cast<float>(std::log2(static_cast<double>(anchorExposure) / static_cast<double>(smallestExposure)));
    }
    const float mergeCeiling = std::max(1.0f, std::pow(2.0f, profile.highlightHeadroomStops));
    std::vector<float> samples;
    std::vector<float> variances;
    std::vector<bool> saturations;
    std::vector<float> weights;
    samples.reserve(mergeFrames.size());
    variances.reserve(mergeFrames.size());
    saturations.reserve(mergeFrames.size());
    weights.reserve(mergeFrames.size());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            samples.clear();
            variances.clear();
            saturations.clear();
            weights.clear();
            for (const MergeFrame& entry : mergeFrames) {
                // The frame's own normalized reading, and the same reading in the anchor's
                // exposure. Saturation belongs to the frame that recorded it, so it is tested
                // before the rescale: a rescaled value cannot tell that its sensel clipped.
                const float own = sample_mosaic(*entry.frame, x, y, entry.alignment->dx, entry.alignment->dy);
                samples.push_back(own * entry.scale);
                const float variance = entry.readNoise * entry.readNoise + entry.shotCoefficient * std::max(0.0f, own);
                variances.push_back(variance * entry.scale * entry.scale);
                saturations.push_back(own >= 0.995f);
            }
            if (samples.empty()) continue;
            // Saturation is decided over the whole set before any weight is assigned. A
            // clipped frame must not keep its weight just because it happened to be visited
            // before the frame that still holds the highlight: excluding every clipped sample
            // and letting the unsaturated frames carry the pixel is what recovers highlight
            // detail that only part of the burst resolved.
            bool hasUnsaturated = false;
            for (const bool saturated : saturations) {
                if (!saturated) { hasUnsaturated = true; break; }
            }
            float firstEstimate = 0.0f;
            float firstWeight = 0.0f;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                // Comparing each frame against the single noisy reference frame with a fixed
                // threshold classifies sensor noise as motion in every textured region, which
                // pins the merge to the reference. The robust reweighting below rejects real
                // outliers against the merged estimate instead.
                float weight = mergeFrames[i].alignment->confidence / std::max(1e-6f, variances[i]);
                if (saturations[i] && hasUnsaturated) weight = 0.0f;
                weights.push_back(weight);
                firstEstimate += samples[i] * weight;
                firstWeight += weight;
            }
            const float initial = firstWeight > 0.0f
                ? firstEstimate / firstWeight
                : sample_mosaic(*mergeFrames.front().frame, x, y, 0.0f, 0.0f);
            float estimate = initial;
            const int iterations = 2;
            for (int iteration = 0; iteration < iterations; ++iteration) {
                float weighted = 0.0f;
                float total = 0.0f;
                float outliers = 0.0f;
                const bool lastIteration = iteration + 1 == iterations;
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    const float robust = robust_weight(std::fabs(samples[i] - estimate), variances[i], profile);
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
            merged.linear[static_cast<std::size_t>(y) * width + x] = std::max(0.0f, std::min(mergeCeiling, estimate));
            confidenceSum += firstWeight > 0.0f ? std::min(1.0f, firstWeight * readNoise * readNoise) : 0.0;
        }
    }
    const double pixelCount = static_cast<double>(width) * height;
    diagnostics.motionFraction = static_cast<float>(motionSum / std::max(1.0, pixelCount * static_cast<double>(accepted)));
    diagnostics.mergeConfidence = static_cast<float>(confidenceSum / std::max(1.0, pixelCount));

    // Recovered highlights. A sample above one means the anchor exposure clipped the sensel but a
    // darker frame in the same burst still resolved it, so the range above one is the burst's real
    // extra highlight range: it exists only because the frames were put into a common exposure,
    // and it is the reason to merge exposures at all. The merge carries that range as it stands,
    // above white, and does not fold it down here: how far above white a highlight really is only
    // becomes clear once white balance and the camera matrix have scaled the channels, so the
    // range is measured here and mapped back inside the display afterwards, by the display white
    // point. A uniform burst never exceeds one, so it reports none and nothing below touches its
    // render.
    if (profile.highlightHeadroomStops > 0.0f && mergeCeiling > 1.0f) {
        std::size_t recovered = 0;
        for (const float value : merged.linear) {
            if (value > 1.0f) ++recovered;
        }
        diagnostics.recoveredHighlightFraction = static_cast<float>(static_cast<double>(recovered) / std::max(1.0, pixelCount));
    }

    // White balance, applied once to the whole merged plane rather than to each frame as it
    // was packed. The gains are mostly above one, so folding them into a plane that stops at
    // the sensor's own white level clipped every sensel whose scaled value passed that level -
    // about the top stop of red - and it made the merge read a half-scale red sensel as a
    // blown one, which then dropped that frame from the merge. Applied here the burst keeps
    // its highlight range, and the saturation the merge tested is the sensor's own.
    const std::array<float, 3> burstGains = normalized[0]->whiteBalance;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            merged.linear[index] *= burstGains[static_cast<std::size_t>(bayer_channel(merged.metadata.bayer, x, y))];
        }
    }

    // Multi-frame detail recovery, measured rather than guessed. Every frame but the
    // reference was resampled with bilinear interpolation on its own colour lattice, and a
    // triangle kernel attenuates exactly the top of the band the merge exists to preserve: at
    // the lattice's own Nyquist frequency a half-sample shift averages two out-of-phase
    // neighbours, so a longer burst buys noise reduction with detail. The reference frame is
    // the one observation never resampled, so its same-phase high-pass measures how much of a
    // coefficient the merge lost, and the deficit is what gets added back - the deficit, not a
    // fixed boost, so a longer or higher-resolution burst that loses more is corrected more.
    // Both sides of the comparison are tested against their own noise first: a coefficient the
    // reference's grain could have produced is not evidence of detail, and a coefficient the
    // merged plane's own noise could have produced is not yet detail worth restoring.
    if (profile.enableSubpixelReconstruction && normalized.size() > 1 && profile.subpixelStrength > 0.0f) {
        const std::uint16_t* referenceSamples = normalized[0]->samples.data();
        std::vector<float> lifted = merged.linear;
        const float strength = clamp01(profile.subpixelStrength);
        const float inverseAccepted = 1.0f / static_cast<float>(accepted);
        const float regularization = clamp01(profile.subpixelRegularization);
        for (std::uint32_t y = 1; y + 1 < height; ++y) {
            for (std::uint32_t x = 1; x + 1 < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                const float centre = merged.linear[index];
                // The primary is a frame like any other, so its reading only compares with the
                // merged plane once it has been carried into the anchor's exposure and through
                // the same white balance gain.
                const float gain = burstGains[static_cast<std::size_t>(bayer_channel(merged.metadata.bayer, x, y))];
                const float primaryOwn = static_cast<float>(referenceSamples[index]) / kNormalizedScale;
                const float referenceValue = primaryOwn * primaryScale * gain;
                // Same-phase neighbours: the CFA phase repeats every two pixels, so this
                // average touches only samples of the centre's own colour.
                float mergedSum = 0.0f;
                float referenceSum = 0.0f;
                int count = 0;
                for (const auto offset : {std::pair<int, int>{-2, 0}, {2, 0}, {0, -2}, {0, 2}}) {
                    const int nx = static_cast<int>(x) + offset.first;
                    const int ny = static_cast<int>(y) + offset.second;
                    if (!in_bounds(width, height, nx, ny)) continue;
                    if (bayer_channel(merged.metadata.bayer, static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny)) != bayer_channel(merged.metadata.bayer, x, y)) continue;
                    const std::size_t neighbour = static_cast<std::size_t>(ny) * width + static_cast<std::size_t>(nx);
                    // The phase test above keeps this on the centre's own colour, so both sides
                    // of the comparison already carry the same white balance gain.
                    mergedSum += merged.linear[neighbour];
                    referenceSum += static_cast<float>(referenceSamples[neighbour]) / kNormalizedScale * primaryScale * gain;
                    ++count;
                }
                if (count == 0) continue;
                const float mergedHigh = centre - mergedSum / static_cast<float>(count);
                const float referenceHigh = referenceValue - referenceSum / static_cast<float>(count);
                // Centre minus the mean of `count` same-colour neighbours carries the grain of
                // `count` + 1 samples, so that sum of variances is the unit both thresholds use.
                const float neighbourFactor = 1.0f + 1.0f / static_cast<float>(count);
                // The noise model belongs to the primary's own exposure, and carrying the
                // primary into the anchor multiplies its variance by scale squared - a frame
                // the merge brightened brings its read noise up with it. Omitting the scale
                // would tell the threshold below that an amplified dark frame is as clean as a
                // metered one, and the deficit it then measures is its own grain.
                const float sensorVariance = (readNoise * readNoise + shotCoefficient * std::max(0.0f, primaryOwn)) * primaryScale * primaryScale;
                const float referenceNoise = std::sqrt(std::max(1e-12f, sensorVariance * neighbourFactor));
                const float mergedNoise = std::sqrt(std::max(1e-12f, sensorVariance * inverseAccepted * neighbourFactor));
                // Soft threshold at two sigma: what a single frame's grain alone can produce is
                // subtracted rather than trusted, and whatever survives is the reference's
                // estimate of the coefficient the merge should have carried.
                const float referenceMagnitude = std::fabs(referenceHigh);
                const float referenceLimit = 2.0f * referenceNoise;
                const float target = referenceMagnitude > referenceLimit ? referenceMagnitude - referenceLimit : 0.0f;
                // Only restore structure the merge itself already found above its own noise: a
                // flat region stays flat instead of borrowing the reference's grain.
                const float confidence = smoothstep(mergedNoise, 3.0f * mergedNoise, std::fabs(mergedHigh));
                const float deficit = std::max(0.0f, target - std::fabs(mergedHigh));
                // A correction across a large gradient would ring against the step it sharpens,
                // so the strongest transitions keep only part of it.
                const float gradient = std::fabs(merged.linear[index + 1] - merged.linear[index - 1]) +
                    std::fabs(merged.linear[index + width] - merged.linear[index - width]);
                const float correctionGain = strength * confidence * (1.0f - regularization * smoothstep(0.10f, 0.45f, gradient));
                // Two contributions, because the two failure modes need different answers. The
                // deficit is what the merge measurably lost, and restoring it cannot overshoot.
                // The fixed term covers the top of the band, where the reference's own grain is
                // larger than the coefficient it is trying to vouch for: there the deficit shrinks
                // to zero and stops being informative, so a bounded gain on the merged high-pass
                // - a sharpening of the merged estimate, not of a single frame - keeps the band
                // alive. Where the deficit is large the merged coefficient is small, so the two
                // terms do not double-count.
                const float direction = referenceHigh < 0.0f ? -1.0f : 1.0f;
                const float kMergedDetailGain = 1.2f;
                const float correction = direction * deficit + kMergedDetailGain * mergedHigh;
                // Clamped from below only: white balance can leave the brightest channel above
                // one, and the tone map is where the display range is decided.
                lifted[index] = std::max(0.0f, centre + correctionGain * correction);
            }
        }
        merged.linear.swap(lifted);
    }

    RGBImage output = demosaic(merged);
    apply_camera_color(output, profile);
    // Only a burst that actually recovered range above white is re-exposed: without recovery the
    // plane holds nothing the display cannot show, and mapping it anyway would move the exposure
    // of every ordinary capture. It runs before the denoise stages because those measure their
    // thresholds against the level the frame is finally rendered at.
    if (diagnostics.recoveredHighlightFraction > 0.0f) {
        diagnostics.highlightWhitePoint = apply_highlight_shoulder(output);
    }
    apply_luma_denoise(output, profile, merged.metadata.iso, diagnostics.acceptedFrames);
    apply_chroma_denoise(output, profile, merged.metadata.iso);
    float toneMapStops = 0.0f;
    apply_local_tone_map(output, profile, toneMapStops);
    apply_tone_and_detail(output, profile);

    float clipped = 0.0f;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float peak = std::max(output.at(x, y, PixelChannel::Red), std::max(output.at(x, y, PixelChannel::Green), output.at(x, y, PixelChannel::Blue)));
            if (peak >= 0.995f) clipped += 1.0f;
        }
    }
    diagnostics.clippedFraction = static_cast<float>(clipped / (static_cast<double>(width) * static_cast<double>(height)));
    diagnostics.toneMapStops = toneMapStops;

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
        << "  \"toneMapStops\": " << diagnostics.toneMapStops << ",\n"
        << "  \"clippedFraction\": " << diagnostics.clippedFraction << ",\n"
        << "  \"exposureRangeStops\": " << diagnostics.exposureRangeStops << ",\n"
        << "  \"recoveredHighlightFraction\": " << diagnostics.recoveredHighlightFraction << ",\n"
        << "  \"highlightWhitePoint\": " << diagnostics.highlightWhitePoint << ",\n"
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
