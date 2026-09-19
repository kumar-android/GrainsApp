#include "gcam_c_api.h"

#include "gcam_engine.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

struct GcamEngine {
    gcam::TuningProfile profile = gcam::default_tuning_profile();
    // Frames are packed as they arrive: the engine keeps a 16-bit normalized plane
    // per frame instead of the sensor RAW, so a 32-frame burst stays affordable.
    std::vector<gcam::PackedFrame> frames;
    std::array<float, 3> burstWhiteBalance = {1.0f, 1.0f, 1.0f};
    gcam::ProcessResult result;
    bool hasResult = false;
    std::string lastError;
};

namespace {

int fail(GcamEngine* engine, const std::exception& error, char* buffer, std::uint32_t size) {
    if (engine != nullptr) engine->lastError = error.what();
    if (buffer != nullptr && size > 0) {
        const std::string message = error.what();
        const std::size_t count = std::min<std::size_t>(message.size(), size - 1U);
        std::memcpy(buffer, message.data(), count);
        buffer[count] = '\0';
    }
    return 0;
}

int success(char* buffer, std::uint32_t size) {
    if (buffer != nullptr && size > 0) buffer[0] = '\0';
    return 1;
}

std::string safe_string(const char* value) {
    return value == nullptr ? std::string{} : std::string(value);
}

} // namespace

extern "C" GcamEngine* gcam_create_engine(void) {
    try {
        return new GcamEngine();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void gcam_destroy_engine(GcamEngine* engine) {
    delete engine;
}

extern "C" int gcam_load_profile(GcamEngine* engine, const char* profile_path, char* error_buffer, uint32_t error_buffer_size) {
    if (engine == nullptr || profile_path == nullptr) {
        std::runtime_error error("gcam_load_profile: null argument");
        return fail(engine, error, error_buffer, error_buffer_size);
    }
    try {
        engine->profile = gcam::load_tuning_profile(profile_path);
        return success(error_buffer, error_buffer_size);
    } catch (const std::exception& error) {
        return fail(engine, error, error_buffer, error_buffer_size);
    }
}

extern "C" int gcam_begin_burst(GcamEngine* engine, char* error_buffer, uint32_t error_buffer_size) {
    if (engine == nullptr) {
        std::runtime_error error("gcam_begin_burst: null engine");
        return fail(engine, error, error_buffer, error_buffer_size);
    }
    engine->frames.clear();
    engine->hasResult = false;
    engine->burstWhiteBalance = {1.0f, 1.0f, 1.0f};
    return success(error_buffer, error_buffer_size);
}

extern "C" int gcam_add_raw_frame(GcamEngine* engine, const GcamRawFrameView* view, char* error_buffer, uint32_t error_buffer_size) {
    if (engine == nullptr || view == nullptr || view->pixels == nullptr) {
        std::runtime_error error("gcam_add_raw_frame: null argument");
        return fail(engine, error, error_buffer, error_buffer_size);
    }
    try {
        const std::size_t expectedPixels = static_cast<std::size_t>(view->width) * view->height;
        const std::size_t minimumStride = static_cast<std::size_t>(view->width) * sizeof(std::uint16_t);
        if (view->width == 0 || view->height == 0 || expectedPixels > view->pixel_count ||
            (view->row_stride_bytes != 0 && static_cast<std::size_t>(view->row_stride_bytes) < minimumStride) ||
            (view->row_stride_bytes % sizeof(std::uint16_t)) != 0U) {
            throw std::invalid_argument("gcam_add_raw_frame: invalid dimensions or pixel count");
        }
        gcam::RawFrame frame;
        frame.metadata.width = view->width;
        frame.metadata.height = view->height;
        frame.metadata.rowStrideBytes = view->row_stride_bytes == 0 ? view->width * 2U : view->row_stride_bytes;
        frame.metadata.bitDepth = view->bit_depth;
        frame.metadata.bayer = static_cast<gcam::BayerPattern>(view->bayer_pattern);
        frame.metadata.blackLevel = view->black_level;
        frame.metadata.whiteLevel = view->white_level;
        frame.metadata.iso = view->iso;
        frame.metadata.exposureTimeSeconds = view->exposure_time_seconds;
        frame.metadata.aperture = view->aperture;
        frame.metadata.colorTemperatureKelvin = view->color_temperature_kelvin;
        frame.metadata.whiteBalance = {view->white_balance[0], view->white_balance[1], view->white_balance[2]};
        frame.metadata.orientation = view->orientation;
        frame.metadata.timestampUnixMicros = view->timestamp_unix_micros;
        frame.metadata.frameIndex = view->frame_index;
        frame.metadata.lensIdentifier = safe_string(view->lens_identifier);
        frame.metadata.sensorIdentifier = safe_string(view->sensor_identifier);
        frame.metadata.optionalMetadata = safe_string(view->optional_metadata);
        frame.pixels.assign(view->pixels, view->pixels + expectedPixels);
        if (!frame.valid()) throw std::invalid_argument("gcam_add_raw_frame: frame metadata failed validation");
        // The white balance for the whole burst comes from its first frame, so the
        // profile must already be loaded when the first frame is added.
        if (engine->frames.empty()) engine->burstWhiteBalance = gcam::burst_white_balance(frame, engine->profile);
        engine->frames.push_back(gcam::pack_frame(frame, engine->profile, engine->burstWhiteBalance));
        return success(error_buffer, error_buffer_size);
    } catch (const std::exception& error) {
        return fail(engine, error, error_buffer, error_buffer_size);
    }
}

extern "C" int gcam_process_burst(GcamEngine* engine, char* error_buffer, uint32_t error_buffer_size) {
    if (engine == nullptr) {
        std::runtime_error error("gcam_process_burst: null engine");
        return fail(engine, error, error_buffer, error_buffer_size);
    }
    try {
        engine->result = gcam::merge_packed_frames(engine->frames, engine->profile);
        engine->hasResult = true;
        return success(error_buffer, error_buffer_size);
    } catch (const std::exception& error) {
        engine->hasResult = false;
        return fail(engine, error, error_buffer, error_buffer_size);
    }
}

extern "C" int gcam_get_result(const GcamEngine* engine, GcamImageView* result, char* error_buffer, uint32_t error_buffer_size) {
    if (engine == nullptr || result == nullptr || !engine->hasResult) {
        std::runtime_error error("gcam_get_result: no processed result");
        return fail(const_cast<GcamEngine*>(engine), error, error_buffer, error_buffer_size);
    }
    result->width = engine->result.image.width;
    result->height = engine->result.image.height;
    result->rgb_linear = engine->result.image.pixels.data();
    result->float_count = static_cast<uint32_t>(engine->result.image.pixels.size());
    return success(error_buffer, error_buffer_size);
}

extern "C" int gcam_get_diagnostics_json(const GcamEngine* engine, char* output_buffer, uint32_t output_buffer_size) {
    if (engine == nullptr || output_buffer == nullptr || output_buffer_size == 0) return 0;
    const std::string json = engine->hasResult ? gcam::diagnostics_json(engine->result.diagnostics) : engine->lastError;
    const std::size_t count = std::min<std::size_t>(json.size(), output_buffer_size - 1U);
    std::memcpy(output_buffer, json.data(), count);
    output_buffer[count] = '\0';
    return json.size() < output_buffer_size ? 1 : 0;
}
