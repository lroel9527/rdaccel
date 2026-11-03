#pragma once

// HEVC encoder/decoder on top of FFmpeg's libavcodec.
//
// Encoder backends are tried in order: NVIDIA NVENC (hardware), then libx265
// (software). VAAPI/QSV need hardware frame contexts and are a TODO.
// The decoder is FFmpeg's built-in HEVC decoder.

#include <memory>
#include <string>
#include <vector>

#include "codec/video_codec.h"

namespace rdacore::platform {

// Returns null and fills `error` if no HEVC encoder could be opened.
// `prefer` may name one backend ("hevc_nvenc", "libx265") to try first.
std::unique_ptr<codec::IHevcEncoder> CreateFfmpegHevcEncoder(const codec::EncoderConfig& cfg,
                                                             const std::string& prefer,
                                                             std::string* error);

std::unique_ptr<codec::IHevcDecoder> CreateFfmpegHevcDecoder(std::string* error);

}  // namespace rdacore::platform
