#include "gcam_rawpack.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace gcam {
namespace {

template <typename T>
void write_integral(std::ostream& out, T value) {
    using U = std::make_unsigned_t<T>;
    U v = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.put(static_cast<char>((v >> (i * 8U)) & static_cast<U>(0xffU)));
    }
}

template <typename T>
T read_integral(std::istream& in) {
    using U = std::make_unsigned_t<T>;
    U value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        const int byte = in.get();
        if (byte == std::char_traits<char>::eof()) {
            throw std::runtime_error("RAWPACK: unexpected end of file");
        }
        value |= static_cast<U>(static_cast<unsigned char>(byte)) << (i * 8U);
    }
    return static_cast<T>(value);
}

void write_float(std::ostream& out, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    write_integral(out, bits);
}

float read_float(std::istream& in) {
    const std::uint32_t bits = read_integral<std::uint32_t>(in);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_string(std::ostream& out, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("RAWPACK: metadata string is too large");
    }
    write_integral<std::uint32_t>(out, static_cast<std::uint32_t>(value.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string read_string(std::istream& in) {
    const std::uint32_t length = read_integral<std::uint32_t>(in);
    if (length > 16U * 1024U * 1024U) {
        throw std::runtime_error("RAWPACK: metadata string is unreasonably large");
    }
    std::string value(length, '\0');
    in.read(value.data(), static_cast<std::streamsize>(length));
    if (in.gcount() != static_cast<std::streamsize>(length)) {
        throw std::runtime_error("RAWPACK: truncated metadata string");
    }
    return value;
}

} // namespace

void write_rawpack(const RawFrame& frame, const std::string& path) {
    if (!frame.valid()) {
        throw std::invalid_argument("RAWPACK: invalid frame");
    }

    const std::uint32_t strideBytes = frame.metadata.rowStrideBytes == 0
        ? frame.metadata.width * static_cast<std::uint32_t>(sizeof(std::uint16_t))
        : frame.metadata.rowStrideBytes;
    const std::size_t minimumStride = static_cast<std::size_t>(frame.metadata.width) * sizeof(std::uint16_t);
    if (static_cast<std::size_t>(strideBytes) < minimumStride || (strideBytes % sizeof(std::uint16_t)) != 0U) {
        throw std::invalid_argument("RAWPACK: row stride is not compatible with uint16 pixels");
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("RAWPACK: cannot open output file: " + path);
    }
    out.write(kRawpackMagic, 8);
    write_integral<std::uint32_t>(out, kRawpackVersion);
    write_integral<std::uint32_t>(out, frame.metadata.width);
    write_integral<std::uint32_t>(out, frame.metadata.height);
    write_integral<std::uint32_t>(out, strideBytes);
    write_integral<std::uint16_t>(out, frame.metadata.bitDepth);
    out.put(static_cast<char>(frame.metadata.bayer));
    out.put('\0');
    write_integral<std::uint16_t>(out, 0U);
    write_float(out, frame.metadata.blackLevel);
    write_float(out, frame.metadata.whiteLevel);
    write_float(out, frame.metadata.iso);
    write_float(out, frame.metadata.exposureTimeSeconds);
    write_float(out, frame.metadata.aperture);
    write_float(out, frame.metadata.colorTemperatureKelvin);
    for (float multiplier : frame.metadata.whiteBalance) {
        write_float(out, multiplier);
    }
    write_integral<std::int32_t>(out, frame.metadata.orientation);
    write_integral<std::int64_t>(out, frame.metadata.timestampUnixMicros);
    write_integral<std::uint32_t>(out, frame.metadata.frameIndex);
    write_string(out, frame.metadata.lensIdentifier);
    write_string(out, frame.metadata.sensorIdentifier);
    write_string(out, frame.metadata.optionalMetadata);

    const std::uint32_t stridePixels = strideBytes / sizeof(std::uint16_t);
    for (std::uint32_t y = 0; y < frame.metadata.height; ++y) {
        for (std::uint32_t x = 0; x < stridePixels; ++x) {
            const std::uint16_t value = x < frame.metadata.width ? frame.at(x, y) : 0U;
            write_integral<std::uint16_t>(out, value);
        }
    }
    if (!out) {
        throw std::runtime_error("RAWPACK: failed while writing: " + path);
    }
}

RawFrame read_rawpack(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("RAWPACK: cannot open input file: " + path);
    }
    char magic[8] = {};
    in.read(magic, 8);
    if (in.gcount() != 8 || std::memcmp(magic, kRawpackMagic, 8) != 0) {
        throw std::runtime_error("RAWPACK: magic mismatch");
    }
    const std::uint32_t version = read_integral<std::uint32_t>(in);
    if (version != kRawpackVersion) {
        throw std::runtime_error("RAWPACK: unsupported version " + std::to_string(version));
    }

    RawFrame frame;
    frame.metadata.width = read_integral<std::uint32_t>(in);
    frame.metadata.height = read_integral<std::uint32_t>(in);
    frame.metadata.rowStrideBytes = read_integral<std::uint32_t>(in);
    frame.metadata.bitDepth = read_integral<std::uint16_t>(in);
    const int bayer = in.get();
    in.get();
    read_integral<std::uint16_t>(in);
    frame.metadata.bayer = static_cast<BayerPattern>(static_cast<std::uint8_t>(bayer));
    frame.metadata.blackLevel = read_float(in);
    frame.metadata.whiteLevel = read_float(in);
    frame.metadata.iso = read_float(in);
    frame.metadata.exposureTimeSeconds = read_float(in);
    frame.metadata.aperture = read_float(in);
    frame.metadata.colorTemperatureKelvin = read_float(in);
    for (float& multiplier : frame.metadata.whiteBalance) {
        multiplier = read_float(in);
    }
    frame.metadata.orientation = read_integral<std::int32_t>(in);
    frame.metadata.timestampUnixMicros = read_integral<std::int64_t>(in);
    frame.metadata.frameIndex = read_integral<std::uint32_t>(in);
    frame.metadata.lensIdentifier = read_string(in);
    frame.metadata.sensorIdentifier = read_string(in);
    frame.metadata.optionalMetadata = read_string(in);

    const std::size_t minimumStride = static_cast<std::size_t>(frame.metadata.width) * sizeof(std::uint16_t);
    if (frame.metadata.width == 0 || frame.metadata.height == 0 ||
        static_cast<std::size_t>(frame.metadata.rowStrideBytes) < minimumStride ||
        (frame.metadata.rowStrideBytes % sizeof(std::uint16_t)) != 0U) {
        throw std::runtime_error("RAWPACK: invalid dimensions or stride");
    }
    const std::uint32_t stridePixels = frame.metadata.rowStrideBytes / sizeof(std::uint16_t);
    frame.pixels.assign(static_cast<std::size_t>(frame.metadata.width) * frame.metadata.height, 0U);
    for (std::uint32_t y = 0; y < frame.metadata.height; ++y) {
        for (std::uint32_t x = 0; x < stridePixels; ++x) {
            const std::uint16_t value = read_integral<std::uint16_t>(in);
            if (x < frame.metadata.width) {
                frame.at(x, y) = value;
            }
        }
    }
    if (!frame.valid()) {
        throw std::runtime_error("RAWPACK: decoded frame failed validation");
    }
    return frame;
}

std::string rawpack_metadata_text(const RawFrame& frame) {
    std::ostringstream out;
    const RawMetadata& m = frame.metadata;
    out << "magic: " << kRawpackMagic << '\n'
        << "version: " << kRawpackVersion << '\n'
        << "width: " << m.width << '\n'
        << "height: " << m.height << '\n'
        << "row_stride_bytes: " << m.rowStrideBytes << '\n'
        << "bit_depth: " << m.bitDepth << '\n'
        << "bayer: " << bayer_pattern_name(m.bayer) << '\n'
        << "black_level: " << std::setprecision(9) << m.blackLevel << '\n'
        << "white_level: " << m.whiteLevel << '\n'
        << "iso: " << m.iso << '\n'
        << "exposure_time_seconds: " << m.exposureTimeSeconds << '\n'
        << "aperture: " << m.aperture << '\n'
        << "color_temperature_kelvin: " << m.colorTemperatureKelvin << '\n'
        << "white_balance: " << m.whiteBalance[0] << ", " << m.whiteBalance[1] << ", " << m.whiteBalance[2] << '\n'
        << "orientation: " << m.orientation << '\n'
        << "timestamp_unix_micros: " << m.timestampUnixMicros << '\n'
        << "frame_index: " << m.frameIndex << '\n'
        << "lens_identifier: " << m.lensIdentifier << '\n'
        << "sensor_identifier: " << m.sensorIdentifier << '\n'
        << "optional_metadata: " << m.optionalMetadata << '\n'
        << "pixel_count: " << frame.pixels.size() << '\n';
    return out.str();
}

} // namespace gcam
