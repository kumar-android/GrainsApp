#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcamEngine GcamEngine;

typedef struct GcamRawFrameView {
    uint32_t width;
    uint32_t height;
    uint32_t row_stride_bytes;
    uint16_t bit_depth;
    uint8_t bayer_pattern;
    float black_level;
    float white_level;
    float iso;
    float exposure_time_seconds;
    float aperture;
    float color_temperature_kelvin;
    float white_balance[3];
    int32_t orientation;
    int64_t timestamp_unix_micros;
    uint32_t frame_index;
    const char* lens_identifier;
    const char* sensor_identifier;
    const char* optional_metadata;
    const uint16_t* pixels;
    uint32_t pixel_count;
} GcamRawFrameView;

typedef struct GcamImageView {
    uint32_t width;
    uint32_t height;
    const float* rgb_linear;
    uint32_t float_count;
} GcamImageView;

GcamEngine* gcam_create_engine(void);
void gcam_destroy_engine(GcamEngine* engine);
int gcam_load_profile(GcamEngine* engine, const char* profile_path, char* error_buffer, uint32_t error_buffer_size);
int gcam_begin_burst(GcamEngine* engine, char* error_buffer, uint32_t error_buffer_size);
int gcam_add_raw_frame(GcamEngine* engine, const GcamRawFrameView* frame, char* error_buffer, uint32_t error_buffer_size);
int gcam_process_burst(GcamEngine* engine, char* error_buffer, uint32_t error_buffer_size);
int gcam_get_result(const GcamEngine* engine, GcamImageView* result, char* error_buffer, uint32_t error_buffer_size);
int gcam_get_diagnostics_json(const GcamEngine* engine, char* output_buffer, uint32_t output_buffer_size);

#ifdef __cplusplus
}
#endif
