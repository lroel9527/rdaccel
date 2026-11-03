#pragma once

// Deterministic synthetic "desktop" used where no real screen exists (CI,
// the headless cloud box) and for repeatable bitrate comparisons.
//
// Each frame shows: a light window background, lines of glyph-like text that
// grow by one character per frame (typing), and a small animated rectangle
// that changes every frame (a video playing in a window).

#include <atomic>
#include <thread>
#include <vector>

#include "capture/capturer.h"

namespace rdaccel::capture {

// Renders frame `index` into `bgra` (resized to w*h*4). Also returns the
// rectangles that differ from frame index-1 (everything for index 0).
std::vector<Rect> RenderSyntheticFrame(uint64_t index, int32_t w, int32_t h,
                                       std::vector<uint8_t>& bgra);

class SyntheticCapturer final : public ICapturer {
 public:
  // `max_frames` = 0 runs until Stop().
  explicit SyntheticCapturer(uint64_t max_frames = 0) : max_frames_(max_frames) {}
  ~SyntheticCapturer() override { Stop(); }

  bool Start(const CaptureConfig& cfg, FrameFn on_frame) override;
  void Stop() override;
  const std::string& last_error() const override { return error_; }

  // Blocks until the capturer has delivered max_frames (only if non-zero).
  void Join();

 private:
  uint64_t max_frames_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::string error_;
};

}  // namespace rdaccel::capture
