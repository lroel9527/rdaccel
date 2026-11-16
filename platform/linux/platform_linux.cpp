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

bool ListInputSources(std::vector<InputSourceInfo>& out, std::string* error) {
  out.clear();
  if (error) *error = "input sources are only supported on macOS";
  return false;
}

bool CurrentInputSource(InputSourceInfo& out, std::string* error) {
  out = {};
  if (error) *error = "input sources are only supported on macOS";
  return false;
}

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

// HEVC only: H.264 (the fallback for slow HEVC hosts, 1.11) is not built on
// Linux, so a Linux viewer does not advertise it and a Linux host keeps HEVC.
std::unique_ptr<codec::IHevcEncoder> CreateVideoEncoder(const codec::EncoderConfig& cfg,
                                                        const std::string& prefer,
                                                        std::string* error) {
  if (cfg.codec != codec::VideoCodec::kHevc) {
    if (error) *error = "H.264 encoding is not supported on Linux";
    return nullptr;
  }
  return CreateFfmpegHevcEncoder(cfg, prefer, error);
}

std::unique_ptr<codec::IHevcDecoder> CreateVideoDecoder(codec::VideoCodec codec, std::string* error) {
  if (codec != codec::VideoCodec::kHevc) {
    if (error) *error = "H.264 decoding is not supported on Linux";
    return nullptr;
  }
  return CreateFfmpegHevcDecoder(error);
}

bool CanDecode(codec::VideoCodec codec) { return codec == codec::VideoCodec::kHevc; }

}  // namespace rdacore::platform
