#include "platform.h"

#include "mac/capture_sck.h"
#include "mac/video_vt.h"

namespace rdaccel::platform {

const char* PlatformName() { return "macOS"; }

std::unique_ptr<capture::ICapturer> CreateScreenCapturer(std::string* /*error*/) {
  return std::make_unique<SckCapturer>(/*exclude_self=*/true);
}

std::unique_ptr<codec::IHevcEncoder> CreateHevcEncoder(const codec::EncoderConfig& cfg,
                                                       const std::string& /*prefer*/,
                                                       std::string* error) {
  return CreateVtHevcEncoder(cfg, error);
}

std::unique_ptr<codec::IHevcDecoder> CreateHevcDecoder(std::string* error) {
  return CreateVtHevcDecoder(error);
}

}  // namespace rdaccel::platform
