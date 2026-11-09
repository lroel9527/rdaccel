#pragma once

// Factory functions implemented once per platform:
//   platform/mac/platform_mac.mm      (macOS)
//   platform/linux/platform_linux.cpp (Linux)
// Apps and tools only include this header, so they build on both.

#include <memory>
#include <string>
#include <vector>

#include "capture/capturer.h"
#include "capture/cursor_source.h"
#include "codec/video_codec.h"
#include "input_injector.h"

namespace rdacore::platform {

// Short name of the platform, e.g. "macOS" or "Linux".
const char* PlatformName();

// The displays that can be captured, primary first (index 0). False with
// `error` set when there is no display at all (no X server, ...).
bool ListDisplays(std::vector<capture::DisplayInfo>& out, std::string* error);

// Captures CaptureConfig::display_index. Returns null with `error` set on
// failure.
std::unique_ptr<capture::ICapturer> CreateScreenCapturer(std::string* error);

// `prefer` optionally names a backend to try first (Linux: "hevc_nvenc",
// "libx265"; ignored on macOS, which always uses VideoToolbox).
std::unique_ptr<codec::IHevcEncoder> CreateHevcEncoder(const codec::EncoderConfig& cfg,
                                                       const std::string& prefer,
                                                       std::string* error);

std::unique_ptr<codec::IHevcDecoder> CreateHevcDecoder(std::string* error);

// The system cursor, relative to display `display_index` (invisible while it
// is on another display). Null with `error` set if it cannot be read (e.g. no
// X display, or no such display).
std::unique_ptr<capture::ICursorSource> CreateCursorSource(int32_t display_index, std::string* error);

// A keyboard input source: a layout (ABC, US) or an input method mode
// (Pinyin, Kana). macOS: Text Input Sources.
struct InputSourceInfo {
  std::string id;        // "com.apple.inputmethod.SCIM.ITABC", "com.apple.keylayout.ABC"
  std::string name;      // localized, for display
  std::string language;  // its first language, BCP 47 ("zh-Hans", "en"); may be empty
  bool input_method = false;  // composes text (Pinyin, Kana) rather than a plain layout
  bool ascii_capable = false; // types ASCII as is (ABC, US)
};

// The keyboard input sources the user enabled and can switch to, and the
// one selected now. False with `error` where the platform has none to offer
// (Linux so far). macOS: needs the main thread or a running main run loop.
bool ListInputSources(std::vector<InputSourceInfo>& out, std::string* error);
bool CurrentInputSource(InputSourceInfo& out, std::string* error);

// Injects input into display cfg.display_index. Null with `error` set when
// the OS will not let this process do it (no XTest; on macOS, no
// Accessibility permission) or there is no such display.
std::unique_ptr<IInputInjector> CreateInputInjector(const InjectorConfig& cfg, std::string* error);

}  // namespace rdacore::platform
