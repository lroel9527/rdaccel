#pragma once

// The receiving side: decodes what a Host sends and hands BGRA pictures to the
// application, which draws them however it likes.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rdacore/connection.h"
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
  // Sound (1.13). audio_bytes is also in bytes_received.
  uint64_t audio_packets = 0;
  uint64_t audio_bytes = 0;
  uint64_t audio_lost = 0;  // frames filled in by loss concealment
  uint64_t audio_late = 0;
  uint64_t audio_underruns = 0;
  int32_t audio_buffer_ms = 0;
  int32_t audio_target_ms = 0;
  int32_t audio_drift_ppm = 0;
  int32_t av_offset_ms = 0;  // sound delay less picture delay; > 0: the sound is later
};

struct ViewerHandlers {
  // The host's screen size, sent once when the stream starts.
  std::function<void(int32_t width, int32_t height)> on_stream_info;

  // A decoded full-screen picture. `changed` lists the areas that differ from
  // the previous picture (empty = everything), so a renderer can upload only
  // those. The image is valid only during the call. Runs on the library's
  // decode thread (as does on_stream_info), so a slow decode or handler
  // never delays the cursor; copy the pixels if you draw on another thread.
  std::function<void(const ImageView& bgra, std::span<const Rect> changed, const FrameInfo& info)>
      on_frame;

  // The host's mouse cursor (see cursor.h): called when the pointer moves,
  // hides or changes its image. Draw state.image with its hotspot at
  // (state.x, state.y) on top of the latest frame, e.g. with DrawCursor() or
  // a hardware cursor. Runs on the transport thread, possibly while on_frame
  // is running: guard state the two share.
  std::function<void(const CursorState& state)> on_cursor;

  // Connection state: kConnected once the host answered the handshake,
  // kDisconnected (final) with the reason. Transport or timer thread.
  std::function<void(ConnectionState state, ConnectionError error)> on_state;

  // Application messages from the host (transport thread).
  std::function<void(uint32_t type, std::span<const uint8_t> payload)> on_message;
};

struct ViewerOptions {
  // No bytes from the host for this long = disconnected (kTimeout).
  // 0 = never time out.
  int32_t peer_timeout_ms = 10'000;
  // Sent to the host's auth hook in the handshake.
  std::vector<uint8_t> auth_token;
  // 32 bytes, the host's key. Empty = plaintext.
  std::vector<uint8_t> encryption_key;
  // Take the host's sound (1.13): only where the transport carries
  // datagrams; read it with ReadAudio().
  bool audio = false;
};

class Viewer {
 public:
  // Takes ownership of the transport. `transport` may be null for playing
  // back recordings with FeedRecording(). Returns null and sets `error` if no
  // HEVC decoder is available.
  static std::unique_ptr<Viewer> Create(std::unique_ptr<Transport> transport,
                                        ViewerHandlers handlers, std::string* error = nullptr);
  static std::unique_ptr<Viewer> Create(std::unique_ptr<Transport> transport,
                                        ViewerHandlers handlers, ViewerOptions options,
                                        std::string* error = nullptr);
  ~Viewer();
  Viewer(const Viewer&) = delete;
  Viewer& operator=(const Viewer&) = delete;

  // Dropped unless connected (the host ignores a viewer it has not accepted).
  // False (with `error`) for a kText event when the host is too old to
  // take text input; nothing is sent then.
  bool SendInput(const InputEvent& ev, std::string* error = nullptr);

  // Reliable, ordered application message to the host (see Host::SendMessage).
  bool SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error = nullptr);
  // On the bulk channel, and what of it waits (see Host::SendBulkMessage).
  bool SendBulkMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error = nullptr);
  int64_t BulkQueuedBytes() const;
  // On the input channel: the host gets it in order with the input events
  // sent before and after it, through its message handler. At most 64 KiB;
  // false with a host older than 3.2 (it does not offer it).
  bool SendInputMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error = nullptr);

  // kConnecting until the host answers; kDisconnected is final. A viewer
  // without a transport (recording playback) stays kConnecting.
  ConnectionState state() const;

  // Asks the host for a full refresh. The viewer does this by itself when
  // it cannot decode what arrives (at most every 250 ms); applications only
  // need it if they lose pictures on their side. The refresh comes with the
  // host's next frame.
  void RequestKeyframe();

  // Decodes a recording (see recording.h) synchronously on this thread.
  void FeedRecording(std::span<const uint8_t> bytes);

  // The host's sound (ViewerOptions::audio): fills `out` with exactly
  // `frames` frames, 48 kHz stereo interleaved float, silence where there is
  // none; returns how many were sound. For an audio render thread: never
  // blocks, allocates or logs. play_time_us: when the first frame reaches the
  // speaker (capture::MonotonicMicros' clock, as rda_now_us), 0 = now. One
  // caller.
  size_t ReadAudio(float* out, size_t frames, uint64_t play_time_us = 0);

  std::string decoder_name() const;
  ViewerStats stats() const;

 private:
  struct Impl;
  explicit Viewer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdacore
