#pragma once

#include "gcam_rawpack.h"
#include "gcam_tuning.h"
#include "gcam_types.h"

#include <string>
#include <vector>

namespace gcam {

ProcessResult process_burst(const std::vector<RawFrame>& frames, const TuningProfile& profile);
std::string diagnostics_json(const ProcessingDiagnostics& diagnostics);
void write_ppm(const RGBImage& image, const std::string& path);
RGBImage read_ppm(const std::string& path);

} // namespace gcam
