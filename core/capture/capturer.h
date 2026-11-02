#pragma once

// Platform-neutral screen capture interface. Implementations:
//   * macOS: ScreenCaptureKit (platform/mac/capture_sck.mm)
//   * Linux: X11 MIT-SHM (platform/linux/capture_x11.cpp)
//   * Any:   synthetic test pattern (core/capture/synthetic_capturer.h)

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rdaccel/geometry.h"
#include "tile/tile_diff.h"

namespace rdaccel::capture {

struct CapturedFrame {
  tile::ImageView bgra;      // valid only during the callback
  std::vector<Rect> dirty;   // empty = whole frame may have changed
  uint64_t capture_time_us;  // monotonic clock, microseconds
};

using FrameFn = std::function<void(const CapturedFrame&)>;

struct CaptureConfig {
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
};

uint64_t MonotonicMicros();

}  // namespace rdaccel::capture
