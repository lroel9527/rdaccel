#include "platform.h"

#include "linux/capture_x11.h"
#include "linux/video_ffmpeg.h"

namespace rdacore::platform {

const char* PlatformName() { return "Linux"; }

std::unique_ptr<capture::ICapturer> CreateScreenCapturer(std::string* /*error*/) {
  return std::make_unique<X11Capturer>();
}

std::unique_ptr<codec::IHevcEncoder> CreateHevcEncoder(const codec::EncoderConfig& cfg,
                                                       const std::string& prefer,
                                                       std::string* error) {
  return CreateFfmpegHevcEncoder(cfg, prefer, error);
}

std::unique_ptr<codec::IHevcDecoder> CreateHevcDecoder(std::string* error) {
  return CreateFfmpegHevcDecoder(error);
}

}  // namespace rdacore::platform
