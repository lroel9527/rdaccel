#include "platform.h"

#include "capture/fallback_capturer.h"
#include "mac/capture_cgstream.h"
#include "mac/capture_sck.h"
#include "mac/video_vt.h"

#import <Foundation/Foundation.h>

#include <chrono>

namespace rdacore::platform {

const char* PlatformName() { return "macOS"; }

// ScreenCaptureKit whenever it works. On macOS 12 and 13 only, CGDisplayStream
// stands behind it for Macs where ScreenCaptureKit starts but never delivers
// a frame (seen on an Intel MacBook Pro with macOS 12.7); from macOS 14,
// where CGDisplayStream is deprecated, ScreenCaptureKit alone.
std::unique_ptr<capture::ICapturer> CreateScreenCapturer(std::string* /*error*/) {
  auto sck = [] { return std::unique_ptr<capture::ICapturer>(new SckCapturer(/*exclude_self=*/true)); };
  if (@available(macOS 14.0, *)) return sck();
  return std::make_unique<capture::FallbackCapturer>(
      sck, "ScreenCaptureKit", [] { return std::unique_ptr<capture::ICapturer>(new CgStreamCapturer()); },
      "CGDisplayStream", std::chrono::milliseconds(1500));
}

std::unique_ptr<codec::IHevcEncoder> CreateHevcEncoder(const codec::EncoderConfig& cfg,
                                                       const std::string& /*prefer*/,
                                                       std::string* error) {
  return CreateVtHevcEncoder(cfg, error);
}

std::unique_ptr<codec::IHevcDecoder> CreateHevcDecoder(std::string* error) {
  return CreateVtHevcDecoder(error);
}

}  // namespace rdacore::platform
