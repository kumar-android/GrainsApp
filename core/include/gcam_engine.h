#pragma once

#include "gcam_rawpack.h"
#include "gcam_tuning.h"
#include "gcam_types.h"

#include <string>
#include <vector>

namespace gcam {

// One normalized input frame, quantized to 16 bits. Alignment luma is derived from it
// on demand rather than stored, and the sensor RAW is released as soon as the frame is
// packed, which keeps a burst at two bytes per pixel per frame. That is what lets a
// full-resolution 32-frame burst fit inside a phone application's memory budget.
struct PackedFrame {
    RawMetadata metadata;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The plane holds the sensor's own normalized signal, one sample per sensel. White
    // balance is not folded in here: it is a gain above one that would push the brighter
    // colour channels past the end of the range the plane can store, and clipping them at
    // the packing step destroys highlight range the merge is supposed to preserve. The
    // gains travel with the frame and are applied once, after the merge.
    std::array<float, 3> whiteBalance = {1.0f, 1.0f, 1.0f};
    std::vector<std::uint16_t> samples;
};

// White balance the whole burst is packed with. Take it from the first frame.
std::array<float, 3> burst_white_balance(const RawFrame& reference, const TuningProfile& profile);

// Normalize one RAW frame with the profile and burst white balance. Packing needs both,
// so load the tuning profile and resolve the burst white balance before adding frames.
PackedFrame pack_frame(const RawFrame& raw, const TuningProfile& profile, const std::array<float, 3>& burstWb);

// Align, merge, and render a packed burst. `ProcessingDiagnostics::processingMilliseconds`
// and `peakMemoryMiB` describe this stage; packing happens before it.
ProcessResult merge_packed_frames(const std::vector<PackedFrame>& frames, const TuningProfile& profile);

// Pack and merge an in-memory burst. Kept for the command-line tools and tests; the
// native engines use pack_frame + merge_packed_frames so they never hold the sensor RAW.
ProcessResult process_burst(const std::vector<RawFrame>& frames, const TuningProfile& profile);
std::string diagnostics_json(const ProcessingDiagnostics& diagnostics);
void write_ppm(const RGBImage& image, const std::string& path);
RGBImage read_ppm(const std::string& path);

} // namespace gcam
