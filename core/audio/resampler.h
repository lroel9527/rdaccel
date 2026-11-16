#pragma once

// Resampling by a ratio close to 1 that may change from call to call: how the
// viewer follows the host's sound clock (the two sound cards differ by tens
// of ppm) without dropping or repeating samples. Polyphase windowed sinc
// (64 taps, Kaiser window, 256 phases with linear interpolation between
// neighbouring phases), cut off at 22 kHz; flat within 0.1 dB to 18 kHz.
//
// Real-time safe: Write and Read neither allocate nor block.

#include <cstddef>
#include <vector>

namespace rdacore::audio {

class Resampler {
 public:
  static constexpr int kTaps = 64;
  static constexpr int kPhases = 256;

  // `capacity` input frames can wait at once.
  explicit Resampler(int channels = 2, size_t capacity = 16384);

  // Back to silence: no input waiting, the history zero.
  void Reset();
  // Input frames waiting (not yet passed by the read position).
  size_t Buffered() const { return end_ - pos_; }
  // Appends input; returns how many frames fit.
  size_t Write(const float* in, size_t frames);
  // Input frames that must be waiting to produce `frames` output frames at
  // `ratio` (input frames per output frame).
  size_t Needed(size_t frames, double ratio) const;
  // Produces up to `frames` output frames at `ratio`; returns how many (fewer
  // when the input runs out).
  size_t Read(float* out, size_t frames, double ratio);

 private:
  void Compact();

  const int channels_;
  std::vector<float> table_;  // (kPhases + 1) x kTaps
  std::vector<float> buf_;    // frames x channels
  size_t capacity_;
  size_t pos_ = 0;   // the frame the next output is centred on (with frac_)
  size_t end_ = 0;   // one past the last frame written
  double frac_ = 0;  // 0..1
};

}  // namespace rdacore::audio
