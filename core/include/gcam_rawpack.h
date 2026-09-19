#pragma once

#include "gcam_types.h"

#include <string>

namespace gcam {

constexpr const char kRawpackMagic[] = "GCAMRAW1";
constexpr std::uint32_t kRawpackVersion = 1;

void write_rawpack(const RawFrame& frame, const std::string& path);
RawFrame read_rawpack(const std::string& path);
std::string rawpack_metadata_text(const RawFrame& frame);

} // namespace gcam
