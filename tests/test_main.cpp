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
