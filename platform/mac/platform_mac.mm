#include "platform.h"

#include "mac/capture_sck.h"
#include "mac/video_vt.h"

namespace rdacore::platform {

const char* PlatformName() { return "macOS"; }

std::unique_ptr<capture::ICapturer> CreateScreenCapturer(std::string* /*error*/) {
  return std::make_unique<SckCapturer>(/*exclude_self=*/true);
}

std::unique_ptr<codec::IHevcEncoder> CreateVideoEncoder(const codec::EncoderConfig& cfg,
                                                        const std::string& /*prefer*/,
                                                        std::string* error) {
  return CreateVtEncoder(cfg, error);
}

std::unique_ptr<codec::IHevcDecoder> CreateVideoDecoder(codec::VideoCodec codec, std::string* error) {
  return CreateVtDecoder(codec, error);
}

// VideoToolbox decodes both on every supported macOS (in software where the
// GPU cannot).
bool CanDecode(codec::VideoCodec) { return true; }

}  // namespace rdacore::platform
