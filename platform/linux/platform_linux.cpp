#include "platform.h"

#include <cstdlib>
#include <cstring>

#include "linux/capture_x11.h"
#ifdef RDACCEL_HAVE_PIPEWIRE
#include "linux/capture_pipewire.h"
#endif
#include "linux/video_ffmpeg.h"

namespace rdacore::platform {

const char* PlatformName() { return "Linux"; }

std::unique_ptr<capture::ICapturer> CreateScreenCapturer(std::string* error) {
  // A Wayland desktop can only be captured through the screen-cast portal.
#ifdef RDACCEL_HAVE_PIPEWIRE
  if (UseWaylandCapture()) return std::make_unique<PipeWireCapturer>();
#else
  const char* v = std::getenv("RDACCEL_CAPTURE");
  if (v && (std::strcmp(v, "pipewire") == 0 || std::strcmp(v, "portal") == 0)) {
    if (error) *error = "rdaccel was built without PipeWire (Wayland) capture";
    return nullptr;
  }
#endif
  (void)error;
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
