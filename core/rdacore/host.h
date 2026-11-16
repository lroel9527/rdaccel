#pragma once

// The sending side: captures (or is given) screen frames, finds what changed,
// encodes it and sends it over a Transport.

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

struct HostConfig {
  int32_t fps = 60;
  int64_t bitrate_bps = 8'000'000;
  // Quality ceiling for the video encoder as a minimum QP (0..51, lower =
  // finer). The default saves about half the video bitrate on desktop content
  // for under 1 dB; 0 lets the encoder decide.
  int32_t min_qp = 24;
  // HEVC encoder backend to try first. Empty picks automatically. macOS always
  // uses VideoToolbox; Linux accepts "hevc_nvenc", "libx265" or
  // "hevc_vaapi" (experimental, never picked automatically).
  std::string encoder;
  // Which video codec to send (rda_host_config.video_codec). kAuto: HEVC,
  // unless the viewer decodes H.264 and this machine's hardware HEVC encoder
  // is missing or below 30 fps at the screen's size while hardware H.264 is
  // faster (measured once per size, a fraction of a second). kHevc: always
  // HEVC, nothing measured. kH264: H.264 for viewers that decode it, HEVC
  // (with a warning) for those that do not. Viewers older than 1.11 always
  // get HEVC.
  enum class VideoCodecPreference : int32_t { kAuto = 0, kHevc = 1, kH264 = 2 };
  VideoCodecPreference video_codec = VideoCodecPreference::kAuto;

  // Only used by StartScreenCapture(). 0 = native resolution. Otherwise the
  // screen is captured scaled to this size (even numbers), and the cursor
  // image is scaled to match.
  int32_t capture_width = 0;
  int32_t capture_height = 0;
  // Which display to capture (DisplayInfo::index from ListDisplays()); 0 =
  // the primary one.
  int32_t display_index = 0;
  // Draw the cursor into the captured picture. Off by default: the cursor
  // channel below is cheaper and smoother.
  bool capture_cursor = false;
  // Send the system cursor on the cursor channel (see cursor.h), polled
  // `cursor_poll_hz` times a second, while StartScreenCapture() runs and
  // `capture_cursor` is off. With SubmitFrame() call SetCursor() instead.
  bool cursor_channel = true;
  int32_t cursor_poll_hz = 120;
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
  // Progressive keyframes: the first picture (and any full refresh) goes out
  // as video only, which is several times smaller and appears quickly even
  // on a slow link; text and UI then turn exact over the next frames, using
  // most of the bitrate, and what stays video gets one sharper picture at the
  // end. Off: the first picture is exact but arrives late.
  bool progressive = true;
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

  // Text mode: the same experience with less traffic. Text and UI that
  // stand still, or change in a few places (typing), are reduced to 16 of
  // their own colours (more where 16 would show; only at edges), coded
  // compactly and sent at once: three to seven times less on text. Motion
  // (a scroll, a zoom to full screen, a film) goes through the video
  // encoder as without text mode, and what is text is sent again exactly
  // once it stops. Video (and the bitrate) is as without text mode; the
  // cursor is sent at most 60 times a second.
  // Host::SetTextMode() switches it while streaming.
  bool text_mode = false;

  // Connection: once a viewer has connected, no bytes from it for this long
  // means it is gone (disconnected, kTimeout). 0 = never time out.
  int32_t peer_timeout_ms = 10'000;
  // When the viewer goes (leaves, times out, is refused, has the wrong
  // key), wait for the next one on the same port: the state goes from
  // kDisconnected back to kConnecting. Screen capture keeps running. Works
  // with BindUdp, BindTcp and unconnected UdpFromFd sockets; with other
  // transports the first disconnect stays final. false = always final.
  bool keep_serving = true;
  // StartScreenCapture(): also capture this process's own windows (macOS
  // leaves them out by default, so a viewer in the same process does not
  // film itself).
  bool capture_own_windows = false;
  // Decides whether a viewer may connect, from the token it sent (on the
  // transport thread). Empty = everyone may.
  std::function<bool(std::span<const uint8_t> token)> auth;
  // 32 bytes shared with the viewer: encrypts and authenticates everything
  // (see transport/secure_transport.h). Empty = plaintext.
  std::vector<uint8_t> encryption_key;
  // Initial state and message handlers (see Host::SetStateHandler), set
  // before the connection can change.
  std::function<void(ConnectionState, ConnectionError)> on_state;
  std::function<void(uint32_t type, std::span<const uint8_t> payload)> on_message;
};

struct HostStats {
  uint64_t frames_captured = 0;
  uint64_t frames_skipped = 0;  // no tile changed
  uint64_t frames_encoded = 0;  // frames sent: video, lossless or both
  uint64_t video_frames = 0;    // frames that went through the video encoder
  uint64_t keyframes = 0;
  uint64_t keyframe_requests = 0;  // from the viewer (it could not decode)
  uint64_t encode_errors = 0;
  uint64_t encoder_drops = 0;  // pictures an encoder (full screen or area) chose not to produce
  uint64_t bytes_sent = 0;      // everything, cursor channel included
  uint64_t video_bytes = 0;     // HEVC payload only
  uint64_t lossless_bytes = 0;  // lossless payload only
  uint64_t lossless_tiles = 0;
  uint64_t moved_tiles = 0;     // sent as copies of what the viewer shows
  uint64_t cached_tiles = 0;    // painted from the viewer's tile cache
  uint64_t refreshed_tiles = 0; // re-sent at better quality once static
  uint64_t progressive_tiles = 0;  // lossless tiles sent after a progressive keyframe
  uint64_t region_pictures = 0;  // pictures of video area streams
  uint64_t region_bytes = 0;     // their HEVC payload (also in video_bytes)
  uint64_t regions_opened = 0;   // video areas detected so far
  uint64_t regions_closed = 0;   // ... and closed after going quiet
  uint64_t region_encode_us_total = 0;
  // Adaptive bitrate (zero unless the transport measures the path).
  uint64_t frames_dropped_congestion = 0;  // skipped because the send queue was full
  // Frames the host re-sent from memory because no new frame came for
  // 100 ms while a keyframe was requested or a skipped frame's changes were
  // still unsent (a static screen; ScreenCaptureKit only delivers changes).
  uint64_t frames_resubmitted = 0;
  uint64_t resolution_changes = 0;  // frames of a new size started a new stream
  int64_t target_bitrate_bps = 0;          // current encoder budget
  int64_t bandwidth_estimate_bps = 0;      // from the transport
  uint32_t rtt_us = 0;
  // Packets the transport sent and lost (resent) for the current viewer;
  // zero unless the transport measures the path (UDP). Sampled with rtt_us.
  uint64_t packets_sent = 0;
  uint64_t packets_lost = 0;
  uint64_t packets_spurious = 0;  // of packets_lost, acknowledged after all (late, not lost)
  uint64_t fec_parity_sent = 0;   // forward error correction packets sent
  uint64_t fec_recovered = 0;     // lost packets the viewer rebuilt from them
  // Cursor channel.
  uint64_t cursor_updates = 0;  // position messages
  uint64_t cursor_images = 0;   // images sent (each distinct image once)
  uint64_t cursor_bytes = 0;
  uint64_t encode_us_total = 0;    // video encoder
  uint64_t lossless_us_total = 0;  // palette extraction + deflate
  // Sound (1.13), cumulative over viewers. audio_bytes (whole packets,
  // headers included) is also in bytes_sent.
  uint64_t audio_packets = 0;
  uint64_t audio_bytes = 0;
  uint64_t audio_frames = 0;        // encoded
  uint64_t audio_frames_gated = 0;  // not sent: silence
  uint64_t audio_red_bytes = 0;     // of audio_bytes, copies of the previous frame
  uint64_t audio_dropped_stale = 0;  // waited too long in the transport
};

// The host's sound (1.13; core/audio/).
struct AudioConfig {
  enum class Source : int32_t { kApp = 0, kSystem = 1, kTest = 2 };
  // kSystem: everything this computer plays (macOS 14.2 Process Tap).
  // kTest: a computed test signal, touching no device. kApp: SubmitAudio.
  Source source = Source::kSystem;
  int32_t profile = 0;  // 0 full (10 ms, 160 kbps), 1 data saver (20 ms, 96 kbps)
  // The computer stays silent while its sound is captured and sent (only
  // once the capture is known to have the permission).
  bool mute_local = false;
  // Applications not to capture (bundle identifiers), besides this process.
  std::vector<std::string> exclude_bundle_ids;
};

enum class AudioState : int32_t {
  kOff = 0,
  kWaitingPeer = 1,  // no viewer, or one that takes no sound
  kStarting = 2,
  kRunning = 3,
  kPaused = 4,
  kNoPermission = 5,
  kFailed = 6,  // retried every 2 s; see message
};

struct AudioStatus {
  AudioState state = AudioState::kOff;
  int32_t profile = 0;
  bool silent = true;  // running, and nothing is sent: the computer is silent
  int32_t bitrate_bps = 0;
  int32_t frame_ms = 0;
  int32_t channels = 0;  // coded: 1 or 2
  bool muted_local = false;  // the capture mutes this computer right now
  std::string message;       // for kFailed and kNoPermission
};

class Host {
 public:
  using InputHandler = std::function<void(const InputEvent&)>;
  using ErrorHandler = std::function<void(const std::string&)>;
  using StateHandler = std::function<void(ConnectionState, ConnectionError)>;
  using MessageHandler = std::function<void(uint32_t type, std::span<const uint8_t> payload)>;

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

  // Connection state changes: kConnected when a compatible viewer said hello,
  // kDisconnected (final) with the reason. Transport or timer thread.
  void SetStateHandler(StateHandler fn);
  ConnectionState state() const;

  // Application messages from the viewer (transport thread), and sending
  // them: reliable, ordered, at most 16 MiB, apart from the picture. Types
  // below kFirstUserMessage are the library's (kClipboardTextMessage).
  void SetMessageHandler(MessageHandler fn);
  bool SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error = nullptr);
  // The same on the bulk channel: after the video, in what the path has to
  // spare (large transfers); on the message channel with a viewer older than
  // 1.10. BulkQueuedBytes: what of it waits to be sent, -1 when not known
  // (TCP, or no bulk channel); a sender keeps it short to stay responsive.
  bool SendBulkMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error = nullptr);
  int64_t BulkQueuedBytes() const;

  // Mode 1: let the library capture the main display (ScreenCaptureKit on
  // macOS, X11 on Linux). Fails without the Screen Recording permission.
  bool StartScreenCapture(std::string* error = nullptr);
  void StopScreenCapture();

  // Mode 2: feed frames yourself (your own capturer, a virtual display, ...).
  // `dirty` is an optional hint of what may have changed (empty = anything);
  // the library still compares tiles, so a coarse hint is fine.
  // `capture_time_us` is any monotonic microsecond clock, e.g. NowMicros().
  // A frame of another size starts a new stream at that size (new encoder,
  // keyframe; the viewer gets on_stream_info again).
  // Returns false (with `error`) if the frame could not be encoded; the next
  // frame then becomes a full refresh. Calls must not overlap.
  bool SubmitFrame(const ImageView& bgra, uint64_t capture_time_us,
                   std::span<const Rect> dirty = {}, std::string* error = nullptr);

  // The mouse cursor, for applications that feed frames themselves (with
  // StartScreenCapture() the library reads the system cursor). Position in
  // pixels of the submitted frames; the image is sent once and remembered,
  // so calling SetCursorImage() with the same image again costs nothing.
  // Thread-safe, independent of SubmitFrame(), any rate: unchanged state is
  // not resent. `time_us` 0 = now.
  void SetCursor(int32_t x, int32_t y, bool visible = true, uint64_t time_us = 0);
  void SetCursorImage(const CursorImage& image);

  // Makes the next encoded frame a full refresh.
  void RequestKeyframe();

  // Turns text mode (HostConfig::text_mode) on or off; takes effect with the
  // next frame. Turning it off refreshes the whole picture with exact
  // colours. Thread-safe.
  void SetTextMode(bool on);
  bool text_mode() const;

  // Sound (1.13). Capture (kSystem, kTest) starts when a viewer that takes
  // sound is connected and stops when it leaves; nothing is captured or sent
  // for one that does not (an older viewer, one that did not ask, TCP).
  // False (with `error`) where the source does not exist (kSystem on Linux)
  // or the library was built without sound.
  bool StartAudio(const AudioConfig& config, std::string* error = nullptr);
  void StopAudio();
  // AudioConfig::Source::kApp: 48 kHz stereo interleaved float. Calls must
  // not overlap. False when no sound is being sent.
  bool SubmitAudio(const float* interleaved, size_t frames, uint64_t capture_time_us);
  void SetAudioProfile(int32_t profile);
  void SetAudioPaused(bool paused);
  AudioStatus audio_status() const;

  // Empty until the first frame has been encoded.
  std::string encoder_name() const;
  HostStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdacore
