#pragma once

// Screen capture on Linux/X11 with the MIT-SHM extension. Works on a real X
// server and on Xvfb (used for the headless cloud tests). X11 gives no dirty
// rects here, so frames are reported with an empty dirty list and the tile
// differ finds the changes. XDamage would provide dirty rects (TODO, M1).

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "capture/capturer.h"

namespace rdaccel::platform {

class X11Capturer final : public capture::ICapturer {
 public:
  X11Capturer() = default;
  ~X11Capturer() override { Stop(); }

  bool Start(const capture::CaptureConfig& cfg, capture::FrameFn on_frame) override;
  void Stop() override;
  const std::string& last_error() const override { return error_; }

 private:
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::string error_;
};

}  // namespace rdaccel::platform
