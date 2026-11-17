#include "platform.h"

#include "mac/capture_sck.h"
#include "mac/video_vt.h"

namespace rdacore::platform {

const char* PlatformName() { return "macOS"; }

std::unique_ptr<capture::ICapturer> CreateScreenCapturer(std::string* /*error*/) {
  return std::make_unique<SckCapturer>(/*exclude_self=*/true);
}

std::unique_ptr<codec::IHevcEncoder> CreateVideoEncoder(const codec::EncoderConfig& cfg, std::string* error) {
  return CreateVtEncoder(cfg, error);
}

std::unique_ptr<codec::IHevcDecoder> CreateVideoDecoder(std::string* error) { return CreateVtDecoder(error); }

}  // namespace rdacore::platform
