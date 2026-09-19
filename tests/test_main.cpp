#include "gcam_engine.h"
#include "gcam_c_api.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace gcam;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

RawFrame make_frame(std::uint32_t width, std::uint32_t height, std::uint32_t index) {
    RawFrame frame;
    frame.metadata.width = width;
    frame.metadata.height = height;
    frame.metadata.rowStrideBytes = width * 2U;
    frame.metadata.bitDepth = 12;
    frame.metadata.bayer = BayerPattern::RGGB;
    frame.metadata.blackLevel = 64.0f;
    frame.metadata.whiteLevel = 4095.0f;
    frame.metadata.iso = 200.0f;
    frame.metadata.whiteBalance = {2.0f, 1.0f, 1.5f};
    frame.metadata.frameIndex = index;
    frame.metadata.lensIdentifier = "test-lens";
    frame.metadata.sensorIdentifier = "test-sensor";
    frame.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            float value = 0.18f + 0.25f * static_cast<float>(x) / static_cast<float>(width) + 0.12f * static_cast<float>(y) / static_cast<float>(height);
            value += 0.025f * std::sin(static_cast<float>(x) * 0.45f) * std::sin(static_cast<float>(y) * 0.31f);
            if (x > width / 3U && x < width / 2U && y > height / 4U && y < height * 3U / 4U) value += 0.24f;
            const std::uint32_t noiseSeed = (x + 17U) * 2654435761U ^ (y + 31U) * 2246822519U ^ (index + 1U) * 3266489917U;
            const float noise = static_cast<float>((noiseSeed >> 8U) & 0xffU) / 255.0f - 0.5f;
            value += noise * 0.025f;
            value = std::max(0.01f, std::min(0.95f, value));
            const PixelChannel channel = bayer_channel(frame.metadata.bayer, x, y);
            const float gain = channel == PixelChannel::Red ? 0.95f : (channel == PixelChannel::Green ? 0.73f : 0.60f);
            frame.at(x, y) = static_cast<std::uint16_t>(std::lround(frame.metadata.blackLevel + value * gain * (frame.metadata.whiteLevel - frame.metadata.blackLevel)));
        }
    }
    return frame;
}

// The same scene recorded at a chosen exposure. A frame the burst underexposed is darker in
// proportion, and a highlight it no longer clips is the only place that frame carries
// information the brightest frame of the burst lost.
RawFrame make_exposure_frame(std::uint32_t width, std::uint32_t height, std::uint32_t index, float exposureScale, bool withHighlights, float highlightLevel = 1.35f) {
    RawFrame frame;
    frame.metadata.width = width;
    frame.metadata.height = height;
    frame.metadata.rowStrideBytes = width * 2U;
    frame.metadata.bitDepth = 12;
    frame.metadata.bayer = BayerPattern::RGGB;
    frame.metadata.blackLevel = 64.0f;
    frame.metadata.whiteLevel = 4095.0f;
    frame.metadata.iso = 200.0f;
    frame.metadata.exposureTimeSeconds = (1.0f / 120.0f) * exposureScale;
    frame.metadata.whiteBalance = {2.0f, 1.0f, 1.5f};
    frame.metadata.frameIndex = index;
    frame.metadata.lensIdentifier = "test-lens";
    frame.metadata.sensorIdentifier = "test-sensor";
    frame.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            float scene = 0.16f + 0.24f * static_cast<float>(x) / static_cast<float>(width) + 0.10f * static_cast<float>(y) / static_cast<float>(height);
            if (withHighlights && x > width / 2U && y > height / 3U && y < height * 2U / 3U) {
                scene = highlightLevel + 0.25f * std::sin(static_cast<float>(x) * 0.6f) * std::sin(static_cast<float>(y) * 0.5f);
            }
            const PixelChannel channel = bayer_channel(frame.metadata.bayer, x, y);
            // The per-channel gains are the inverse of the white balance the metadata carries,
            // so the highlight stays neutral - a bright sky - and the white balance leaves its
            // levels where the scene put them. A red-hot highlight would sit above the display
            // range in red alone however it was tone mapped, which is a different problem from
            // the one this frame exists to exercise.
            const float gain = channel == PixelChannel::Red ? 0.5f : (channel == PixelChannel::Green ? 1.0f : 1.0f / 1.5f);
            const float value = std::max(0.0f, std::min(1.0f, scene * gain * exposureScale));
            frame.at(x, y) = static_cast<std::uint16_t>(std::lround(frame.metadata.blackLevel + value * (frame.metadata.whiteLevel - frame.metadata.blackLevel)));
        }
    }
    return frame;
}

double mean_absolute_difference(const RGBImage& a, const RGBImage& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) sum += std::fabs(a.pixels[i] - b.pixels[i]);
    return a.pixels.empty() ? 0.0 : sum / static_cast<double>(a.pixels.size());
}

// Standard deviation of the highlight patch in one channel. The channel matters: in the
// synthetic highlight below the metered exposure clips red flat while green and blue keep
// their slope, so the luma spread of that patch is carried by the channels that never
// clipped and says nothing about whether anything was recovered. Red is the channel that
// tells the two bursts apart, so it is the one the test measures.
double highlight_patch_spread(const RGBImage& image, PixelChannel channel) {
    double sum = 0.0;
    double sumSquares = 0.0;
    std::size_t count = 0;
    for (std::uint32_t y = image.height / 3U + 1U; y < image.height * 2U / 3U - 1U; ++y) {
        for (std::uint32_t x = image.width / 2U + 1U; x < image.width - 1U; ++x) {
            const double value = image.at(x, y, channel);
            sum += value;
            sumSquares += value * value;
            ++count;
        }
    }
    if (count == 0) return 0.0;
    const double mean = sum / static_cast<double>(count);
    return std::sqrt(std::max(0.0, sumSquares / static_cast<double>(count) - mean * mean));
}

// A burst whose frames re-metered must merge to the exposure the camera metered, not to some
// average of the exposures it happened to use. Nothing else about the burst changes, so the
// two merges have to agree.
void test_exposure_normalized_merge() {
    std::vector<RawFrame> uniform;
    std::vector<RawFrame> bracketed;
    for (std::uint32_t i = 0; i < 4; ++i) uniform.push_back(make_exposure_frame(64, 48, i, 1.0f, false));
    bracketed.push_back(make_exposure_frame(64, 48, 0, 1.0f, false));
    bracketed.push_back(make_exposure_frame(64, 48, 1, 1.0f, false));
    bracketed.push_back(make_exposure_frame(64, 48, 2, 0.25f, false));
    bracketed.push_back(make_exposure_frame(64, 48, 3, 0.25f, false));
    TuningProfile profile = default_tuning_profile();
    const ProcessResult reference = process_burst(uniform, profile);
    const ProcessResult measured = process_burst(bracketed, profile);
    require(reference.diagnostics.exposureRangeStops < 0.01f, "a uniform burst spans no exposure range");
    require(std::fabs(measured.diagnostics.exposureRangeStops - 2.0f) < 0.05f, "measured exposure range in stops");
    require(mean_absolute_difference(reference.image, measured.image) < 0.01, "an underexposed frame must merge at the metered exposure");
}

// The reason to bracket: a highlight the metered exposure clipped is still in the frames it
// underexposed, and the merge has to carry that range and roll it off instead of pinning it
// to white. A uniform burst has nothing to recover, so it must report none.
void test_bracketed_highlight_recovery() {
    std::vector<RawFrame> uniform;
    std::vector<RawFrame> bracketed;
    for (std::uint32_t i = 0; i < 6; ++i) {
        uniform.push_back(make_exposure_frame(64, 48, i, 1.0f, true));
        bracketed.push_back(make_exposure_frame(64, 48, i, i < 3U ? 1.0f : 0.25f, true));
    }
    TuningProfile profile = default_tuning_profile();
    const ProcessResult clipped = process_burst(uniform, profile);
    const ProcessResult recovered = process_burst(bracketed, profile);
    require(clipped.diagnostics.recoveredHighlightFraction == 0.0f, "a uniform burst holds no range above white");
    require(recovered.diagnostics.recoveredHighlightFraction > 0.001f, "a bracketed burst carries range above white");
    // Green is the channel the metered exposure clips first, so it is the one that comes back
    // with texture only if the burst carried the range.
    const double clippedSpread = highlight_patch_spread(clipped.image, PixelChannel::Green);
    const double recoveredSpread = highlight_patch_spread(recovered.image, PixelChannel::Green);
    require(recoveredSpread > clippedSpread * 2.0, "a recovered highlight must keep its texture instead of clipping flat");
}

// Carrying range above white is only worth anything if the render puts it back inside the
// display range: a highlight the merged plane holds far above white must come out below white,
// with its texture, instead of being pinned there by the clamp every later stage applies. The
// metered burst clips that highlight in every frame, so it has nothing to recover: it must keep
// its own exposure rather than being re-exposed by a white point that only the bracket earned.
void test_recovered_highlight_stays_below_white() {
    std::vector<RawFrame> uniform;
    std::vector<RawFrame> bracketed;
    for (std::uint32_t i = 0; i < 6; ++i) {
        uniform.push_back(make_exposure_frame(64, 48, i, 1.0f, true, 3.2f));
        bracketed.push_back(make_exposure_frame(64, 48, i, i < 3U ? 1.0f : 0.25f, true, 3.2f));
    }
    TuningProfile profile = default_tuning_profile();
    const ProcessResult clipped = process_burst(uniform, profile);
    const ProcessResult recovered = process_burst(bracketed, profile);
    require(recovered.diagnostics.recoveredHighlightFraction > 0.001f, "the bracket recovered range above white");
    require(clipped.diagnostics.highlightWhitePoint == 1.0f, "a burst with nothing above white keeps its metered exposure");
    require(recovered.diagnostics.highlightWhitePoint > 1.5f, "a recovered highlight moves the display white point");
    // Not zero: the very top of the recovered range still compresses toward white, as it must.
    // The claim is that recovering the range keeps the highlight out of the clamp, not that the
    // clamp disappears.
    const float clippedShare = clipped.diagnostics.clippedFraction;
    require(recovered.diagnostics.clippedFraction < clippedShare * 0.2f, "a recovered highlight is not pinned at white");
    const double clippedSpread = highlight_patch_spread(clipped.image, PixelChannel::Green);
    const double recoveredSpread = highlight_patch_spread(recovered.image, PixelChannel::Green);
    require(recoveredSpread > clippedSpread * 2.0, "a recovered highlight keeps its texture below white");
}

void test_bayer_patterns() {
    require(bayer_channel(BayerPattern::RGGB, 0, 0) == PixelChannel::Red, "RGGB origin");
    require(bayer_channel(BayerPattern::BGGR, 0, 0) == PixelChannel::Blue, "BGGR origin");
    require(bayer_channel(BayerPattern::GRBG, 1, 0) == PixelChannel::Red, "GRBG red site");
    require(bayer_channel(BayerPattern::GBRG, 0, 1) == PixelChannel::Red, "GBRG red site");
}

void test_rawpack_roundtrip() {
    const RawFrame input = make_frame(12, 10, 7);
    RawFrame padded = input;
    padded.metadata.rowStrideBytes = padded.metadata.width * 2U + 8U;
    const fs::path path = fs::temp_directory_path() / "gcam_rawpack_roundtrip.rawpack";
    write_rawpack(padded, path.string());
    const RawFrame output = read_rawpack(path.string());
    require(output.metadata.width == padded.metadata.width, "RAWPACK width roundtrip");
    require(output.metadata.height == padded.metadata.height, "RAWPACK height roundtrip");
    require(output.metadata.rowStrideBytes == padded.metadata.rowStrideBytes, "RAWPACK padded stride roundtrip");
    require(output.metadata.bayer == padded.metadata.bayer, "RAWPACK Bayer roundtrip");
    require(output.metadata.lensIdentifier == padded.metadata.lensIdentifier, "RAWPACK string roundtrip");
    require(output.pixels == padded.pixels, "RAWPACK pixels must be lossless with row padding");
    fs::remove(path);
}

void test_deterministic_processing_and_motion() {
    std::vector<RawFrame> burst;
    for (std::uint32_t i = 0; i < 8; ++i) burst.push_back(make_frame(64, 48, i));
    TuningProfile profile = default_tuning_profile();
    profile.maxFrames = 12;
    const ProcessResult first = process_burst(burst, profile);
    const ProcessResult second = process_burst(burst, profile);
    require(first.image.width == 64 && first.image.height == 48, "processed dimensions");
    require(first.image.pixels == second.image.pixels, "CPU pipeline must be deterministic");
    require(first.diagnostics.acceptedFrames >= 1, "at least one frame must be accepted");
    require(first.diagnostics.motionFraction >= 0.0f && first.diagnostics.motionFraction <= 1.0f, "motion fraction range");
    for (float value : first.image.pixels) require(std::isfinite(value) && value >= 0.0f && value <= 1.0f, "output must be finite and bounded");
}

void test_profile_changes_output() {
    std::vector<RawFrame> burst;
    for (std::uint32_t i = 0; i < 5; ++i) burst.push_back(make_frame(40, 32, i));
    TuningProfile restrained = default_tuning_profile();
    restrained.fineSharpen = 0.0f;
    restrained.microcontrastAmount = 0.0f;
    TuningProfile detailed = restrained;
    detailed.fineSharpen = 0.45f;
    detailed.microcontrastAmount = 0.30f;
    const ProcessResult a = process_burst(burst, restrained);
    const ProcessResult b = process_burst(burst, detailed);
    double difference = 0.0;
    for (std::size_t i = 0; i < a.image.pixels.size(); ++i) difference += std::fabs(a.image.pixels[i] - b.image.pixels[i]);
    require(difference > 0.01, "tuning parameters must materially affect output");
}

void test_c_api() {
    GcamEngine* engine = gcam_create_engine();
    require(engine != nullptr, "C API engine allocation");
    char error[512] = {};
    require(gcam_begin_burst(engine, error, sizeof(error)) == 1, "C API begin burst");
    RawFrame frame = make_frame(16, 12, 0);
    GcamRawFrameView view{};
    view.width = frame.metadata.width;
    view.height = frame.metadata.height;
    view.row_stride_bytes = frame.metadata.rowStrideBytes;
    view.bit_depth = frame.metadata.bitDepth;
    view.bayer_pattern = static_cast<std::uint8_t>(frame.metadata.bayer);
    view.black_level = frame.metadata.blackLevel;
    view.white_level = frame.metadata.whiteLevel;
    view.iso = frame.metadata.iso;
    view.white_balance[0] = frame.metadata.whiteBalance[0];
    view.white_balance[1] = frame.metadata.whiteBalance[1];
    view.white_balance[2] = frame.metadata.whiteBalance[2];
    view.pixels = frame.pixels.data();
    view.pixel_count = static_cast<std::uint32_t>(frame.pixels.size());
    require(gcam_add_raw_frame(engine, &view, error, sizeof(error)) == 1, "C API add frame");
    require(gcam_process_burst(engine, error, sizeof(error)) == 1, "C API process");
    GcamImageView result{};
    require(gcam_get_result(engine, &result, error, sizeof(error)) == 1, "C API result");
    require(result.width == frame.metadata.width && result.height == frame.metadata.height, "C API result dimensions");
    gcam_destroy_engine(engine);
}

} // namespace

int main() {
    try {
        test_bayer_patterns();
        test_exposure_normalized_merge();
        test_bracketed_highlight_recovery();
        test_recovered_highlight_stays_below_white();
        test_rawpack_roundtrip();
        test_deterministic_processing_and_motion();
        test_profile_changes_output();
        test_c_api();
        std::cout << "gcam_tests: all tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gcam_tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
