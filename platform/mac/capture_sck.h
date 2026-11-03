#pragma once

// Screen capture on macOS with ScreenCaptureKit (macOS 14+). Captures one
// display (CaptureConfig::display_index) at native pixel resolution in BGRA and reports SCK's dirty
// rects. Needs the Screen Recording permission (System Settings > Privacy &
// Security > Screen & System Audio Recording).

#include <memory>
#include <string>

#include "capture/capturer.h"

namespace rdacore::platform {

class SckCapturer final : public capture::ICapturer {
 public:
  // `exclude_self`: leave this process's own windows out of the capture, so
  // a loopback viewer does not film itself.
  explicit SckCapturer(bool exclude_self);
  ~SckCapturer() override;

  bool Start(const capture::CaptureConfig& cfg, capture::FrameFn on_frame) override;
  void Stop() override;
  const std::string& last_error() const override { return error_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool exclude_self_;
  std::string error_;
};

}  // namespace rdacore::platform
