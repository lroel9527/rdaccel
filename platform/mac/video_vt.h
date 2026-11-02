#pragma once

// HEVC encoder/decoder on VideoToolbox (Apple Silicon hardware).

#include <memory>
#include <string>

#include "codec/video_codec.h"

namespace rdaccel::platform {

// Hardware HEVC encoder with low-latency rate control when available. Returns
// null and fills `error` if the session cannot be created.
std::unique_ptr<codec::IHevcEncoder> CreateVtHevcEncoder(const codec::EncoderConfig& cfg,
                                                         std::string* error);

std::unique_ptr<codec::IHevcDecoder> CreateVtHevcDecoder(std::string* error);

}  // namespace rdaccel::platform
