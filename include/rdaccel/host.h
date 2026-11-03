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
  // Quality ceiling for the video encoder as a minimum QP (0..51, lower =
  // finer). The default saves about half the video bitrate on desktop content
  // for under 1 dB; 0 lets the encoder decide.
  int32_t min_qp = 24;
  // HEVC encoder backend to try first. Empty picks automatically. macOS always
  // uses VideoToolbox; Linux accepts "hevc_nvenc" or "libx265".
  std::string encoder;

  // Only used by StartScreenCapture(). 0 = native resolution.
  int32_t capture_width = 0;
  int32_t capture_height = 0;
  bool capture_cursor = false;  // the viewer is expected to draw the cursor
  // Send tiles with few colours (text, UI) through the lossless layer instead
  // of the video encoder. A tile that also changed in the previous frame
  // (animation, video, dragging) is "hot" and must have at most
  // `lossless_max_colors_hot` colours; other tiles, and small slivers such as
  // the rows a scroll uncovers, may have up to `lossless_max_colors`.
  bool lossless = true;
  int32_t lossless_max_colors = 256;
  int32_t lossless_max_colors_hot = 4;
  // Detect scrolling and moved windows and send them as copy commands.
  bool detect_moves = true;
  // Video frames during which a static tile keeps receiving the encoder's
  // quality refinements after it changed. 0 = only send changed tiles.
  int32_t refine_frames = 8;
  // Let the viewer keep tiles that are likely to come back (a window that
  // gets uncovered, a cycling animation) and repaint them from memory.
  bool tile_cache = true;
  // Re-send areas left at first-pass video quality (e.g. content that
  // scrolled in) once they stop changing. Off by default: on the benchmark
  // scenes it cost more than it gained (see README).
  bool refresh = false;
  // Give areas that behave like video (many neighbouring tiles that need the
  // video encoder nearly every frame: a movie, a game) their own HEVC stream,
  // cropped to the area. The full-screen encoder then only handles the rest
  // of the screen, the area gets its own bitrate and quality, and encoding
  // a small video costs a small encode instead of a full-screen one.
  bool video_regions = true;
  // Quality floor (minimum QP) inside video areas. Motion hides coarser
  // quantisation, so this is higher than `min_qp`.
  int32_t video_region_min_qp = 27;
  // Bitrate of each video area's stream; 0 = a share of `bitrate_bps`
  // proportional to the area (at least a quarter of it).
  int64_t video_region_bitrate_bps = 0;

  // Adaptive bitrate, for transports that measure the path (GetStats(), e.g.
  // the UDP transport): the encoders' bitrate follows the estimated
  // bandwidth (between `min_bitrate_bps` and `bitrate_bps`), and frames are
  // skipped while the send queue would take more than `max_queue_ms` to
  // drain, so a slow link costs frame rate instead of latency.
  bool adaptive_bitrate = true;
  int64_t min_bitrate_bps = 300'000;
  int32_t max_queue_ms = 50;
};

struct HostStats {
  uint64_t frames_captured = 0;
  uint64_t frames_skipped = 0;  // no tile changed
  uint64_t frames_encoded = 0;  // frames sent: video, lossless or both
  uint64_t video_frames = 0;    // frames that went through the video encoder
  uint64_t keyframes = 0;
  uint64_t encode_errors = 0;
  uint64_t bytes_sent = 0;
  uint64_t video_bytes = 0;     // HEVC payload only
  uint64_t lossless_bytes = 0;  // lossless payload only
  uint64_t lossless_tiles = 0;
  uint64_t moved_tiles = 0;     // sent as copies of what the viewer shows
  uint64_t cached_tiles = 0;    // painted from the viewer's tile cache
  uint64_t refreshed_tiles = 0; // re-sent at better quality once static
  uint64_t region_pictures = 0;  // pictures of video area streams
  uint64_t region_bytes = 0;     // their HEVC payload (also in video_bytes)
  uint64_t regions_opened = 0;   // video areas detected so far
  uint64_t regions_closed = 0;   // ... and closed after going quiet
  uint64_t region_encode_us_total = 0;
  // Adaptive bitrate (zero unless the transport measures the path).
  uint64_t frames_dropped_congestion = 0;  // skipped because the send queue was full
  int64_t target_bitrate_bps = 0;          // current encoder budget
  int64_t bandwidth_estimate_bps = 0;      // from the transport
  uint32_t rtt_us = 0;
  uint64_t encode_us_total = 0;    // video encoder
  uint64_t lossless_us_total = 0;  // palette extraction + deflate
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
  // Returns false (with `error`) if the frame could not be encoded; the next
  // frame then becomes a full refresh. Calls must not overlap.
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
