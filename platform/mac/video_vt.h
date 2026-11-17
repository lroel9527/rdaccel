#pragma once

// HEVC encoder/decoder on VideoToolbox.

#include <memory>
#include <string>

#include "codec/video_codec.h"

namespace rdacore::platform {

// An HEVC encoder: hardware with low-latency rate control when available,
// then hardware, then any (software).
// Returns null and fills `error` if no session works.
std::unique_ptr<codec::IHevcEncoder> CreateVtEncoder(const codec::EncoderConfig& cfg, std::string* error);

std::unique_ptr<codec::IHevcDecoder> CreateVtDecoder(std::string* error);

}  // namespace rdacore::platform
