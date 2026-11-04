#pragma once

// Platform-neutral screen capture interface. Implementations:
//   * macOS: ScreenCaptureKit (platform/mac/capture_sck.mm)
//   * Linux: X11 MIT-SHM (platform/linux/capture_x11.cpp)
//   * Any:   synthetic test pattern (core/capture/synthetic_capturer.h)

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rdacore/geometry.h"
#include "tile/tile_diff.h"

namespace rdacore::capture {

struct CapturedFrame {
  tile::ImageView bgra;      // valid only during the callback
  std::vector<Rect> dirty;   // empty = whole frame may have changed
  uint64_t capture_time_us;  // monotonic clock, microseconds
};

using FrameFn = std::function<void(const CapturedFrame&)>;
// Capture ended on its own after a successful Start (permission revoked,
// display gone, screen sharing stopped, X server gone): why.
using StoppedFn = std::function<void(const std::string& why)>;

// One display (monitor) of the desktop. Index 0 is the primary display.
struct DisplayInfo {
  int32_t index = 0;
  // Origin on the desktop, in the OS's desktop units (points on macOS,
  // pixels on X11); size in pixels.
  int32_t x = 0, y = 0;
  int32_t width = 0, height = 0;
  bool primary = false;
  std::string name;  // UTF-8, may be empty
};

struct CaptureConfig {
  // Which display (DisplayInfo::index); 0 = the primary one.
  int32_t display_index = 0;
  int32_t fps = 60;
  // 0 = native resolution of the display.
  int32_t width = 0;
  int32_t height = 0;
  bool show_cursor = false;  // the viewer draws the cursor itself
};

class ICapturer {
 public:
  virtual ~ICapturer() = default;

  // Starts delivering frames on a capturer-owned thread. Returns false and
  // sets last_error() on failure (e.g. missing Screen Recording permission).
  virtual bool Start(const CaptureConfig& cfg, FrameFn on_frame) = 0;
  virtual void Stop() = 0;
  virtual const std::string& last_error() const = 0;

  // Set before Start. Called at most once, on a capturer thread, when
  // capture ends by itself; not after Stop().
  void SetStoppedHandler(StoppedFn fn) { on_stopped_ = std::move(fn); }

 protected:
  StoppedFn on_stopped_;
};

uint64_t MonotonicMicros();

}  // namespace rdacore::capture
