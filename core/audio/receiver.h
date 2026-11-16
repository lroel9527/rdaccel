#pragma once

// The viewer's sound, from packets (audio_packet.h) to the speaker:
//
//   OnPacket (transport thread) -> lock-free ring -> Read (the audio render
//   thread): jitter buffer -> Opus decode / loss concealment -> resampler
//   (drift) -> out
//
//   * Target delay: from the packets' relative delays d = (arrival - first
//     arrival) - (ts - first ts) / 48 kHz: the 99th percentile of the last
//     5 s less the minimum of the last 10 s, plus 5 ms, within 20..60 ms
//     (30 to begin with; up to 200 ms for 30 s after an underrun). Kept
//     across spurts.
//   * A spurt starts playing once a target's worth has arrived (or the spurt
//     ended, or a target's time has passed).
//   * A missing frame: Opus loss concealment, at most 5 in a row; then a
//     fade to silence, an underrun, and the spurt waits for a target again.
//   * END, or 100 ms without packets: back to idle (silence).
//   * Too much buffered (max(3 x target, target + 100 ms) for 500 ms, after a
//     network stall): the excess is skipped at once with a 5 ms crossfade.
//     Otherwise nothing is ever dropped or repeated:
//   * Clock drift: every 100 ms, with L = the buffer over the last second
//     and e = L - target (ms): ppm = clamp(150 e + I, +-5000), I += 5 e
//     (within +-500); the resampler plays 1 + ppm / 10^6 input samples per
//     output sample. 0.5% at most is under 9 cents, and it changes slowly.
//     The buffer rises by a frame when a packet comes and falls in between;
//     sampled at a fixed rate that saw-tooth aliases (a bias of up to half a
//     frame that wanders with the drift), so L is taken at each packet's
//     arrival (as it was then) less half a frame: the mean level.
//   * Sound against picture (NoteFrameShown): when the sound runs over 20 ms
//     ahead of the picture for 2 s, the target grows by the difference (at
//     most 120 ms more), and shrinks again by 10 ms a second once it no
//     longer is ahead. Nothing while the picture stands still.
//
// Read never allocates, blocks or logs; everything it uses is allocated in
// the constructor.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "audio/resampler.h"

struct OpusDecoder;

namespace rdacore::audio {

struct ReceiverStats {
  uint64_t packets = 0, bytes = 0;  // arrived
  uint64_t lost = 0;       // frames filled in by loss concealment
  uint64_t late = 0;       // frames that came after their time, or twice (not counting RED copies)
  uint64_t underruns = 0;  // ran dry in the middle of a spurt
  uint64_t catchups = 0;   // times the excess of a stall was skipped
  uint64_t dropped = 0;    // packets the ring had no room for
  int32_t buffer_ms = 0, target_ms = 0, drift_ppm = 0;
  int32_t av_offset_ms = 0;  // sound delay less picture delay; > 0: sound later
};

class Receiver {
 public:
  Receiver();
  ~Receiver();
  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  // One datagram; transport thread (one at a time). arrival_us: now.
  void OnPacket(std::span<const uint8_t> bytes, uint64_t arrival_us);
  // Fills exactly `frames` stereo frames; returns how many were sound.
  // play_time_us: when the first one reaches the speaker; now_us: now (both
  // capture::MonotonicMicros' clock). One caller.
  size_t Read(float* out, size_t frames, uint64_t play_time_us, uint64_t now_us);
  // The viewer showed a picture captured at capture_time_us (host clock) at
  // shown_us (here). Any thread.
  void NoteFrameShown(uint64_t capture_time_us, uint64_t shown_us);

  ReceiverStats stats() const;

 private:
  struct Slot;   // ring entry
  struct Frame;  // jitter buffer entry
  void Drain(uint64_t now);
  void Insert(int64_t ts, uint8_t spurt, std::span<const uint8_t> opus, uint64_t arrival, bool copy);
  void NoteDelay(int64_t ts, uint64_t arrival);
  void UpdateTarget(uint64_t now);
  bool TryStart(uint64_t now);
  void DecodeNext(uint64_t now);
  void Stop(bool underrun, uint64_t now);
  void CatchUp();
  void Drift(uint64_t now);
  Frame* Find(int64_t ts, int spurt);
  int64_t SpurtEnd(int spurt) const;  // end of the newest frame of `spurt`, or -1
  double BufferMs() const;
  void ResetDecoder();

  // Ring: written by OnPacket, read by Read.
  static constexpr size_t kRingSlots = 64;
  std::unique_ptr<Slot[]> ring_;
  std::atomic<size_t> ring_write_{0}, ring_read_{0};

  // Everything below: Read's thread only.
  static constexpr size_t kFrames = 64;
  std::unique_ptr<Frame[]> frames_;
  OpusDecoder* dec_ = nullptr;
  Resampler resampler_;
  std::vector<float> pcm_;  // decode scratch
  std::vector<float> fade_;  // crossfade scratch
  bool playing_ = false;
  int cur_spurt_ = -1;
  int dec_spurt_ = -1;  // the spurt the decoder state belongs to
  int64_t play_ts_ = 0;  // next sample position to decode
  int last_samples_ = 480;
  int plc_run_ = 0;
  int end_spurt_ = -1;       // the spurt an END was seen for
  int64_t end_ts_ = 0;       // ... and where it ends
  uint64_t last_arrival_ = 0;
  bool flushing_ = false;  // resampler tail after a spurt ended
  bool played_any_ = false;  // play_ts_ means something: earlier positions are past
  float softclip_[2] = {};   // opus_pcm_soft_clip's state
  // Unwrapping ts (32-bit on the wire).
  bool have_ts_ = false;
  int64_t last_ts_ = 0;
  // Relative delays, for the target.
  struct Delay {
    uint64_t time;
    int64_t d;  // us
  };
  static constexpr size_t kDelays = 2048;
  std::vector<Delay> delays_;
  size_t delays_head_ = 0, delays_count_ = 0;
  std::vector<int64_t> scratch_;
  bool have_first_ = false;
  uint64_t first_arrival_ = 0;
  int64_t first_ts_ = 0;
  double target_ms_ = 30;
  double jitter_target_ms_ = 30;
  uint64_t last_target_update_ = 0;
  uint64_t last_underrun_ = 0;
  // Too much buffered since (0 = not).
  uint64_t over_since_ = 0;
  // Drift: the level at each packet's arrival over the last second.
  size_t out_since_tick_ = 0;
  struct Level {
    uint64_t time;
    double ms;
  };
  static constexpr size_t kLevels = 256;
  Level levels_[kLevels] = {};
  size_t level_head_ = 0, level_count_ = 0;
  double level_ms_ = 0;  // the last second's mean
  double integral_ = 0, ppm_ = 0;
  void NoteLevel(int samples, uint64_t arrival, uint64_t now);
  // A/V: the picture's delays (NoteFrameShown), and how far ahead the sound runs.
  std::atomic<int64_t> video_delay_us_{INT64_MIN};  // 1 s median, here - host clock
  std::atomic<uint64_t> video_last_us_{0};
  struct VideoSample {
    uint64_t time;
    int64_t d;
  };
  std::vector<VideoSample> video_;  // under video_mu_ (NoteFrameShown only)
  size_t video_head_ = 0, video_count_ = 0;
  std::atomic<bool> video_lock_{false};
  // Host clock time of the sample position (from CLOCK).
  bool have_clock_ = false;
  int64_t clock_ts_ = 0;
  uint64_t clock_time_ = 0;
  double av_extra_ms_ = 0;
  uint64_t ahead_since_ = 0;
  bool skew_valid_ = false;
  double skew_ms_ = 0;  // sound delay less picture delay, at the last Read

  std::atomic<uint64_t> packets_{0}, bytes_{0}, lost_{0}, late_{0}, underruns_{0}, catchups_{0}, dropped_{0};
  std::atomic<int32_t> buffer_ms_{0}, target_ms_out_{30}, drift_ppm_{0}, av_offset_ms_{0};
};

}  // namespace rdacore::audio
