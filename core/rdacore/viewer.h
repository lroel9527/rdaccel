#pragma once

// The receiving side: decodes what a Host sends and hands BGRA pictures to the
// application, which draws them however it likes.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "rdacore/cursor.h"
#include "rdacore/geometry.h"
#include "rdacore/image.h"
#include "rdacore/input.h"
#include "rdacore/transport.h"

namespace rdacore {

struct FrameInfo {
  uint64_t frame_id = 0;
  uint64_t capture_time_us = 0;  // host clock
};

struct ViewerStats {
  uint64_t frames_decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t bytes_received = 0;
  uint64_t decode_us_total = 0;
  uint64_t keyframe_requests = 0;  // sent to the host after decode errors
};

struct ViewerHandlers {
  // The host's screen size, sent once when the stream starts.
  std::function<void(int32_t width, int32_t height)> on_stream_info;

  // A decoded full-screen picture. `changed` lists the areas that differ from
  // the previous picture (empty = everything), so a renderer can upload only
  // those. The image is valid only during the call. Runs on the transport
  // thread; copy the pixels if you draw on another thread.
  std::function<void(const ImageView& bgra, std::span<const Rect> changed, const FrameInfo& info)>
      on_frame;

  // The host's mouse cursor (see cursor.h): called when the pointer moves,
  // hides or changes its image. Draw state.image with its hotspot at
  // (state.x, state.y) on top of the latest frame, e.g. with DrawCursor() or
  // a hardware cursor. Runs on the transport thread, possibly between frames.
  std::function<void(const CursorState& state)> on_cursor;
};

class Viewer {
 public:
  // Takes ownership of the transport. `transport` may be null for playing
  // back recordings with FeedRecording(). Returns null and sets `error` if no
  // HEVC decoder is available.
  static std::unique_ptr<Viewer> Create(std::unique_ptr<Transport> transport,
                                        ViewerHandlers handlers, std::string* error = nullptr);
  ~Viewer();
  Viewer(const Viewer&) = delete;
  Viewer& operator=(const Viewer&) = delete;

  void SendInput(const InputEvent& ev);

  // Asks the host for a full refresh. The viewer does this by itself when
  // it cannot decode what arrives (at most every 250 ms); applications only
  // need it if they lose pictures on their side. The refresh comes with the
  // host's next frame.
  void RequestKeyframe();

  // Decodes a recording (see recording.h) synchronously on this thread.
  void FeedRecording(std::span<const uint8_t> bytes);

  std::string decoder_name() const;
  ViewerStats stats() const;

 private:
  struct Impl;
  explicit Viewer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdacore
