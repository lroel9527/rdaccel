#pragma once

// Factory functions implemented once per platform:
//   platform/mac/platform_mac.mm      (macOS)
//   platform/linux/platform_linux.cpp (Linux)
// Apps and tools only include this header, so they build on both.

#include <memory>
#include <string>

#include "capture/capturer.h"
#include "capture/cursor_source.h"
#include "codec/video_codec.h"

namespace rdaccel::platform {

// Short name of the platform, e.g. "macOS" or "Linux".
const char* PlatformName();

// Captures the main display. Returns null with `error` set on failure.
std::unique_ptr<capture::ICapturer> CreateScreenCapturer(std::string* error);

// `prefer` optionally names a backend to try first (Linux: "hevc_nvenc",
// "libx265"; ignored on macOS, which always uses VideoToolbox).
std::unique_ptr<codec::IHevcEncoder> CreateHevcEncoder(const codec::EncoderConfig& cfg,
                                                       const std::string& prefer,
                                                       std::string* error);

std::unique_ptr<codec::IHevcDecoder> CreateHevcDecoder(std::string* error);

// The system cursor of the main display. Null with `error` set if it cannot
// be read (e.g. no X display).
std::unique_ptr<capture::ICursorSource> CreateCursorSource(std::string* error);

}  // namespace rdaccel::platform
