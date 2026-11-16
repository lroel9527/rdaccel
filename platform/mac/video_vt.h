#pragma once

// HEVC and H.264 encoder/decoder on VideoToolbox.

#include <memory>
#include <string>

#include "codec/video_codec.h"

namespace rdacore::platform {

// An encoder for cfg.codec: hardware with low-latency rate control when
// available, then hardware, then any (software) unless cfg.hardware_only.
// Returns null and fills `error` if no session works.
std::unique_ptr<codec::IHevcEncoder> CreateVtEncoder(const codec::EncoderConfig& cfg, std::string* error);

std::unique_ptr<codec::IHevcDecoder> CreateVtDecoder(codec::VideoCodec codec, std::string* error);

}  // namespace rdacore::platform
