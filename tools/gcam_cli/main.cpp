#include "gcam_engine.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace gcam;

namespace {

void usage() {
    std::cout <<
        "gcam_cli - deterministic RAW burst laboratory\n\n"
        "Commands:\n"
        "  generate-synthetic <directory> [--frames N] [--width N] [--height N]\n"
        "  info <rawpack>\n"
        "  process <burst-directory|rawpack|manifest> --profile <xml> --output <ppm> [--diagnostics <json>]\n"
        "  benchmark <dataset-directory> --profile <xml> --output <directory>\n"
        "  compare <reference.ppm> <output.ppm>\n"
        "  export <rawpack> <destination.rawpack>\n";
}

std::string value_after(const std::vector<std::string>& args, const std::string& name, const std::string& fallback = {}) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) return args[i + 1];
    }
    return fallback;
}

std::uint32_t uint_after(const std::vector<std::string>& args, const std::string& name, std::uint32_t fallback) {
    const std::string value = value_after(args, name);
    if (value.empty()) return fallback;
    return static_cast<std::uint32_t>(std::stoul(value));
}

std::uint32_t hash_value(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

RawFrame synthetic_frame(std::uint32_t width, std::uint32_t height, std::uint32_t frameIndex, std::uint32_t frameCount) {
    RawFrame frame;
    frame.metadata.width = width;
    frame.metadata.height = height;
    frame.metadata.rowStrideBytes = width * 2U;
    frame.metadata.bitDepth = 12;
    frame.metadata.bayer = BayerPattern::RGGB;
    frame.metadata.blackLevel = 64.0f;
    frame.metadata.whiteLevel = 4095.0f;
    frame.metadata.iso = 200.0f;
    frame.metadata.exposureTimeSeconds = 1.0f / 120.0f;
    frame.metadata.aperture = 1.78f;
    frame.metadata.colorTemperatureKelvin = 5200.0f;
    frame.metadata.whiteBalance = {2.0f, 1.0f, 1.55f};
    frame.metadata.orientation = 1;
    frame.metadata.frameIndex = frameIndex;
    frame.metadata.lensIdentifier = "synthetic-main-wide";
    frame.metadata.sensorIdentifier = "synthetic-bayer-rggb";
    frame.metadata.optionalMetadata = "deterministic synthetic burst; no camera framework involved";
    frame.pixels.resize(static_cast<std::size_t>(width) * height);

    const float center = (static_cast<float>(frameCount) - 1.0f) * 0.5f;
    const float shiftX = (static_cast<float>(frameIndex) - center) * 0.33f;
    const float shiftY = (static_cast<float>(frameIndex) - center) * -0.19f;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float sx = (static_cast<float>(x) + shiftX) / static_cast<float>(width);
            const float sy = (static_cast<float>(y) + shiftY) / static_cast<float>(height);
            float scene = 0.08f + 0.30f * sx + 0.22f * sy;
            scene += 0.035f * std::sin(sx * 37.0f) * std::sin(sy * 29.0f);
            if (sx > 0.22f && sx < 0.46f && sy > 0.26f && sy < 0.70f) scene += 0.24f;
            if (frameIndex == frameCount / 2U && sx > 0.68f && sx < 0.85f && sy > 0.35f && sy < 0.62f) scene += 0.20f;
            scene = std::max(0.005f, std::min(0.96f, scene));
            const PixelChannel channel = bayer_channel(frame.metadata.bayer, x, y);
            const float channelGain = channel == PixelChannel::Red ? 0.98f : (channel == PixelChannel::Green ? 0.74f : 0.62f);
            scene *= channelGain;
            const std::uint32_t seed = hash_value(x * 73856093U ^ y * 19349663U ^ frameIndex * 83492791U);
            const float n1 = static_cast<float>(seed & 0xffffU) / 65535.0f;
            const float n2 = static_cast<float>((seed >> 16U) & 0xffffU) / 65535.0f;
            const float noise = (n1 + n2 - 1.0f) * std::sqrt(std::max(0.0001f, scene) * 0.025f) + (n1 - 0.5f) * 0.004f;
            if (frameIndex == frameCount - 1U && x > width / 2U && y > height / 3U && y < height * 2U / 3U) scene += 0.20f;
            const float value = std::max(0.0f, std::min(1.0f, scene + noise));
            frame.at(x, y) = static_cast<std::uint16_t>(std::lround(frame.metadata.blackLevel + value * (frame.metadata.whiteLevel - frame.metadata.blackLevel)));
        }
    }
    return frame;
}

std::vector<RawFrame> load_burst(const fs::path& path) {
    std::vector<fs::path> files;
    if (fs::is_directory(path)) {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file() && (entry.path().extension() == ".rawpack" || entry.path().extension() == ".RAWPACK")) files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
    } else if (path.extension() == ".burst") {
        std::ifstream manifest(path);
        if (!manifest) throw std::runtime_error("Cannot open burst manifest: " + path.string());
        std::string line;
        while (std::getline(manifest, line)) {
            if (line.empty() || line[0] == '#') continue;
            fs::path child = fs::path(line);
            if (child.is_relative()) child = path.parent_path() / child;
            files.push_back(child);
        }
    } else {
        files.push_back(path);
    }
    if (files.empty()) throw std::runtime_error("No RAWPACK frames found in " + path.string());
    std::vector<RawFrame> frames;
    frames.reserve(files.size());
    for (const fs::path& file : files) frames.push_back(read_rawpack(file.string()));
    return frames;
}

void write_diagnostics(const ProcessingDiagnostics& diagnostics, const fs::path& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write diagnostics: " + path.string());
    out << diagnostics_json(diagnostics);
}

float image_luma(const RGBImage& image, std::uint32_t x, std::uint32_t y) {
    return 0.2126f * image.at(x, y, PixelChannel::Red) + 0.7152f * image.at(x, y, PixelChannel::Green) + 0.0722f * image.at(x, y, PixelChannel::Blue);
}

void compare_images(const fs::path& referencePath, const fs::path& outputPath) {
    const RGBImage reference = read_ppm(referencePath.string());
    const RGBImage output = read_ppm(outputPath.string());
    if (reference.width != output.width || reference.height != output.height) throw std::runtime_error("compare: dimensions differ");
    double squared = 0.0;
    double lumaSquared = 0.0;
    double edgeReference = 0.0;
    double edgeOutput = 0.0;
    const std::size_t count = reference.pixels.size();
    for (std::size_t i = 0; i < count; ++i) {
        const double difference = static_cast<double>(reference.pixels[i]) - output.pixels[i];
        squared += difference * difference;
    }
    for (std::uint32_t y = 1; y + 1 < reference.height; ++y) {
        for (std::uint32_t x = 1; x + 1 < reference.width; ++x) {
            const float a = image_luma(reference, x, y);
            const float b = image_luma(output, x, y);
            lumaSquared += static_cast<double>(a - b) * (a - b);
            edgeReference += std::fabs(image_luma(reference, x + 1, y) - image_luma(reference, x - 1, y));
            edgeOutput += std::fabs(image_luma(output, x + 1, y) - image_luma(output, x - 1, y));
        }
    }
    const double mse = squared / static_cast<double>(std::max<std::size_t>(1, count));
    const double psnr = mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
    std::cout << "pixels: " << count / 3U << "\n"
              << "psnr_db: " << psnr << "\n"
              << "luma_rmse: " << std::sqrt(lumaSquared / std::max(1.0, static_cast<double>((reference.width - 2U) * (reference.height - 2U)))) << "\n"
              << "edge_energy_ratio: " << (edgeReference > 0.0 ? edgeOutput / edgeReference : 0.0) << "\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage();
            return 2;
        }
        const std::string command = argv[1];
        std::vector<std::string> args;
        for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

        if (command == "generate-synthetic") {
            if (args.empty()) throw std::runtime_error("generate-synthetic needs a destination directory");
            const fs::path destination = args[0];
            const std::uint32_t frames = uint_after(args, "--frames", 8);
            const std::uint32_t width = uint_after(args, "--width", 128);
            const std::uint32_t height = uint_after(args, "--height", 96);
            fs::create_directories(destination);
            for (std::uint32_t i = 0; i < frames; ++i) {
                write_rawpack(synthetic_frame(width, height, i, frames), (destination / ("frame_" + std::to_string(i) + ".rawpack")).string());
            }
            std::cout << "generated " << frames << " deterministic RAWPACK frames at " << destination.string() << "\n";
            return 0;
        }

        if (command == "info") {
            if (args.size() != 1) throw std::runtime_error("info needs one RAWPACK path");
            std::cout << rawpack_metadata_text(read_rawpack(args[0]));
            return 0;
        }

        if (command == "process") {
            if (args.empty()) throw std::runtime_error("process needs a burst path");
            const fs::path input = args[0];
            const fs::path profilePath = value_after(args, "--profile", "config/gcam_natural.xml");
            const fs::path outputPath = value_after(args, "--output", "output.ppm");
            const std::string diagnosticsPath = value_after(args, "--diagnostics");
            const auto frames = load_burst(input);
            const ProcessResult result = process_burst(frames, load_tuning_profile(profilePath.string()));
            fs::create_directories(outputPath.parent_path().empty() ? fs::path(".") : outputPath.parent_path());
            write_ppm(result.image, outputPath.string());
            if (!diagnosticsPath.empty()) write_diagnostics(result.diagnostics, diagnosticsPath);
            std::cout << "burst frames: " << result.diagnostics.inputFrames << "\n"
                      << "dimensions: " << result.diagnostics.width << " x " << result.diagnostics.height << "\n"
                      << "accepted frames: " << result.diagnostics.acceptedFrames << "\n"
                      << "rejected frames: " << result.diagnostics.rejectedFrames << "\n"
                      << "alignment confidence: " << result.diagnostics.meanAlignmentConfidence << "\n"
                      << "motion fraction: " << result.diagnostics.motionFraction << "\n"
                      << "merge confidence: " << result.diagnostics.mergeConfidence << "\n"
                      << "processing time ms: " << result.diagnostics.processingMilliseconds << "\n"
                      << "peak memory MiB: " << result.diagnostics.peakMemoryMiB << "\n"
                      << "output: " << outputPath.string() << "\n";
            return 0;
        }

        if (command == "benchmark") {
            if (args.empty()) throw std::runtime_error("benchmark needs a dataset directory");
            const fs::path dataset = args[0];
            const fs::path profilePath = value_after(args, "--profile", "config/gcam_natural.xml");
            const fs::path output = value_after(args, "--output", "benchmark/out");
            fs::create_directories(output);
            const TuningProfile profile = load_tuning_profile(profilePath.string());
            std::uint32_t count = 0;
            for (const auto& entry : fs::directory_iterator(dataset)) {
                if (!entry.is_directory()) continue;
                try {
                    const ProcessResult result = process_burst(load_burst(entry.path()), profile);
                    const fs::path base = output / ("scene_" + std::to_string(count));
                    write_ppm(result.image, (base.string() + ".ppm"));
                    write_diagnostics(result.diagnostics, (base.string() + ".json"));
                    ++count;
                } catch (const std::exception& error) {
                    std::cerr << "benchmark skipped " << entry.path().string() << ": " << error.what() << '\n';
                }
            }
            std::cout << "benchmarked scenes: " << count << "\n";
            return 0;
        }

        if (command == "compare") {
            if (args.size() != 2) throw std::runtime_error("compare needs reference and output PPM paths");
            compare_images(args[0], args[1]);
            return 0;
        }

        if (command == "export") {
            if (args.size() != 2) throw std::runtime_error("export needs input and output paths");
            const RawFrame frame = read_rawpack(args[0]);
            write_rawpack(frame, args[1]);
            std::cout << "exported lossless RAWPACK frame to " << args[1] << '\n';
            return 0;
        }

        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "gcam_cli error: " << error.what() << '\n';
        return 1;
    }
}
