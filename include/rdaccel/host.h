#pragma once

// The sending side: captures (or is given) screen frames, finds what changed,
// encodes it and sends it over a Transport.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "rdaccel/geometry.h"
#include "rdaccel/image.h"
#include "rdaccel/input.h"
#include "rdaccel/transport.h"

namespace rdaccel {

struct HostConfig {
  int32_t fps = 60;
  int64_t bitrate_bps = 8'000'000;
  // HEVC encoder backend to try first. Empty picks automatically. macOS always
  // uses VideoToolbox; Linux accepts "hevc_nvenc" or "libx265".
  std::string encoder;

  // Only used by StartScreenCapture(). 0 = native resolution.
  int32_t capture_width = 0;
  int32_t capture_height = 0;
  bool capture_cursor = false;  // the viewer is expected to draw the cursor
};

struct HostStats {
  uint64_t frames_captured = 0;
  uint64_t frames_skipped = 0;  // no tile changed
  uint64_t frames_encoded = 0;
  uint64_t keyframes = 0;
  uint64_t bytes_sent = 0;
  uint64_t encode_us_total = 0;
};

class Host {
 public:
  using InputHandler = std::function<void(const InputEvent&)>;
  using ErrorHandler = std::function<void(const std::string&)>;

  // Takes ownership of the transport.
  explicit Host(std::unique_ptr<Transport> transport, HostConfig config = {});
  ~Host();
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // Input events from the viewer, delivered on the transport thread.
  void SetInputHandler(InputHandler fn);

  // Errors that happen on a background thread (e.g. the encoder cannot be
  // created for the captured resolution).
  void SetErrorHandler(ErrorHandler fn);

  // Mode 1: let the library capture the main display (ScreenCaptureKit on
  // macOS, X11 on Linux). Fails without the Screen Recording permission.
  bool StartScreenCapture(std::string* error = nullptr);
  void StopScreenCapture();

  // Mode 2: feed frames yourself (your own capturer, a virtual display, ...).
  // `dirty` is an optional hint of what may have changed (empty = anything);
  // the library still compares tiles, so a coarse hint is fine.
  // `capture_time_us` is any monotonic microsecond clock, e.g. NowMicros().
  // The first frame fixes the resolution; frames of another size are rejected.
  // Calls must not overlap.
  bool SubmitFrame(const ImageView& bgra, uint64_t capture_time_us,
                   std::span<const Rect> dirty = {}, std::string* error = nullptr);

  // Makes the next encoded frame a full refresh.
  void RequestKeyframe();

  // Empty until the first frame has been encoded.
  std::string encoder_name() const;
  HostStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdaccel
