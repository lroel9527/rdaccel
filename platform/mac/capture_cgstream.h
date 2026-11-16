#pragma once

// Screen capture on macOS with CGDisplayStream: the older API, used only on
// macOS 12 and 13 when ScreenCaptureKit gives no picture (see
// capture/fallback_capturer.h and platform_mac.mm). Apple deprecated it in
// macOS 14, where only ScreenCaptureKit is used. It cannot leave windows
// out: it captures everything on the display. Needs the same Screen
// Recording permission.

#include <memory>
#include <string>

#include "capture/capturer.h"

namespace rdacore::platform {

class CgStreamCapturer final : public capture::ICapturer {
 public:
  CgStreamCapturer();
  ~CgStreamCapturer() override;

  bool Start(const capture::CaptureConfig& cfg, capture::FrameFn on_frame) override;
  void Stop() override;
  const std::string& last_error() const override { return error_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string error_;
};

}  // namespace rdacore::platform
