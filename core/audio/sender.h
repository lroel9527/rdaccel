#pragma once

// The host's sound, from PCM to packets (audio_packet.h):
//
//   48 kHz stereo float -> whole frames (10 ms, data saver 20 ms) -> silence
//   gate -> mono when left and right are identical -> Opus (CELT, restricted
//   low delay, VBR) -> packet -> SendFn (one transport datagram each)
//
//   * Silence gate: a frame whose samples are all below 2^-15 (the smallest
//     step of 16-bit audio, -90.3 dBFS) is silent. After 200 ms of silent
//     frames (still sent: no flapping at the threshold) the last one carries
//     END and nothing more is encoded or sent. The frame that brings sound
//     back starts a new spurt: the encoder starts afresh, and the packet
//     carries that frame plus the silent frame before it (RED: the decoder,
//     which also starts afresh, gets the same lead-in, so the attack is
//     whole) and the capture time (CLOCK); so do the next four. Those five
//     frames are coded without inter-frame prediction, so a receiver that
//     lacks the lead-in (lost, or played already as the END frame) decodes
//     them cleanly from a fresh state.
//   * Left = right for every sample for 500 ms of a spurt: coded as mono (at
//     the mono rate) until the first sample that differs, which is coded as
//     stereo again. The receiver always decodes stereo; a mono packet comes out as
//     left = right, exactly what went in.
//   * RED on every packet while the path loses 1% or more of its packets
//     (SetPath); off again after 30 s below 0.3%.
//   * Paused (the viewer cannot hear it): nothing encoded or sent; sound
//     after it starts a new spurt.
//   * A gap in the input (a source that missed callbacks or stopped calling
//     back): silence for that long, the sample positions follow the capture
//     time (any gap over a quarter frame and over the previous chunk).
//
// Push() and everything it calls run on one thread (the sound thread); the
// setters and stats() are thread-safe.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "audio/audio_packet.h"
#include "audio/audio_source.h"

struct OpusEncoder;

namespace rdacore::audio {

inline constexpr int kProfileFull = 0;   // 10 ms frames, 160 kbps stereo
inline constexpr int kProfileSaver = 1;  // 20 ms frames, 96 kbps stereo
// Below this a frame is silent: 2^-15.
inline constexpr float kSilenceThreshold = 1.0f / 32768.0f;

// The encoder's target for a profile and channel count (before a slow path
// lowers it).
int ProfileBitrate(int profile, int channels);
int ProfileFrameSamples(int profile);

struct SenderStats {  // cumulative
  uint64_t packets = 0, bytes = 0;        // sent; bytes = whole packets, header included
  uint64_t frames = 0, frames_gated = 0;  // encoded (main frames); not sent for silence
  uint64_t red_bytes = 0;                 // of `bytes`, the copies of previous frames
};

struct SenderStatus {
  int bitrate_bps = 0;  // the encoder's target now
  int frame_ms = 0;
  int channels = 2;  // coded
  bool silent = true;  // nothing is being sent for silence
};

class Sender {
 public:
  using SendFn = std::function<bool(std::span<const uint8_t>)>;
  Sender(SendFn send, int profile);
  ~Sender();
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  // 48 kHz stereo interleaved; time_us = capture time of the first sample.
  void Push(const float* interleaved, size_t frames, uint64_t time_us);

  void SetProfile(int profile);  // from the next frame
  void SetPaused(bool paused);
  // About once a second: the real loss rate over the last 5 seconds, the
  // transport's bandwidth estimate (0 = none, or not trustworthy yet), and
  // whether the path is full now (a standing queue at the bottleneck). The
  // bitrate comes down for a slow path only while it is full or loses
  // packets, and goes back up otherwise.
  void SetPath(double loss_fraction, int64_t bandwidth_bps, bool congested = false);

  SenderStats stats() const;
  SenderStatus status() const;

 private:
  void Frame(const float* pcm, uint64_t time_us);
  int Encode(const float* pcm, uint8_t* out);  // bytes, or <= 0
  void Configure(int channels);
  void SendPacket(std::span<const uint8_t> main, std::span<const uint8_t> red, bool end, uint64_t time_us);
  void AdaptToPath();

  SendFn send_;
  OpusEncoder* enc_ = nullptr;

  // Sound thread.
  int frame_samples_;  // per channel, of the frame being collected
  std::vector<float> frame_;  // being collected
  size_t fill_ = 0;
  uint64_t frame_time_ = 0;
  uint64_t next_time_ = 0;  // when the next pushed sample is due; 0 = nothing pushed yet
  uint64_t last_chunk_us_ = 0;  // how long the last Push was
  uint32_t ts_ = 0;  // the next frame's first sample
  std::vector<float> prev_;  // the last frame (lead-in for a spurt)
  bool have_prev_ = false;
  bool sending_ = false;   // within a spurt
  int silent_ms_ = 0;       // consecutive silent frames while sending
  int same_ms_ = 0;         // consecutive frames with left = right
  bool mono_ = false;
  bool was_paused_ = false;
  uint8_t spurt_ = 0;
  uint16_t seq_ = 0;
  int spurt_packets_ = 0;  // sent in this spurt
  bool prediction_off_ = false;  // the spurt's first frames: coded on their own
  int since_clock_ = 0;    // packets since the last CLOCK
  std::vector<uint8_t> last_opus_;  // the previous frame's packet, for RED
  bool last_opus_valid_ = false;
  std::vector<uint8_t> packet_, opus_, red_opus_;
  // What the encoder is set to.
  int applied_channels_ = 0, applied_bitrate_ = 0, applied_loss_ = -1;
  // The path (SetPath), applied once a second on the sound thread.
  bool red_on_ = false;
  uint64_t low_loss_since_ = 0;  // sound clock, us
  int path_cap_bps_ = 0;          // 0 = none; a slow path's limit
  uint64_t path_raise_time_ = 0;
  uint64_t path_checked_ = 0;

  std::atomic<int> profile_;
  std::atomic<bool> paused_{false};
  std::atomic<double> loss_{0};
  std::atomic<int64_t> bandwidth_{0};
  std::atomic<bool> congested_{false};
  std::atomic<uint64_t> packets_{0}, bytes_{0}, frames_{0}, frames_gated_{0}, red_bytes_{0};
  std::atomic<int> status_bitrate_{0}, status_frame_ms_{10}, status_channels_{2};
  std::atomic<bool> status_silent_{true};
};

}  // namespace rdacore::audio
