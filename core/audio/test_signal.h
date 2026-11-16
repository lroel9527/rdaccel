#pragma once

// Computed test sound (48 kHz, stereo, interleaved float). Nothing is ever
// recorded or played: tests and the test source (CreateTestSource) feed these
// to the sender directly.

#include <cstddef>
#include <cstdint>

namespace rdacore::audio {

// The test loop, 4 seconds:
//   0.0-0.3 s  1 kHz sine at -20 dBFS, left = right
//   0.3-1.5 s  silence (exact zeros)
//   1.5-2.5 s  440 Hz left, 660 Hz right, -20 dBFS
//   2.5-2.6 s  noise at -95 dBFS (below the silence gate)
//   2.6-3.0 s  1 kHz at -70 dBFS (quiet, above the gate)
//   3.0-4.0 s  silence
// `first` is the position of out[0] in frames from the loop's start.
inline constexpr int64_t kTestLoopFrames = 4 * 48000;
void TestLoop(int64_t first, size_t frames, float* out);

// A sine on both channels (or `right_hz` on the right when it is not 0).
void Sine(int64_t first, size_t frames, double hz, double amplitude, float* out, double right_hz = 0);
// Twelve tones of frequencies that are not multiples of one another, from
// 110 Hz to 15 kHz, summed and scaled to `amplitude` peak; slightly different
// phases left and right.
void Multitone(int64_t first, size_t frames, double amplitude, float* out);
// Logarithmic sweep from 20 Hz to 20 kHz over `length` frames.
void Sweep(int64_t first, size_t frames, size_t length, double amplitude, float* out);
// Uniform white noise of peak `amplitude`, both channels independent.
void Noise(uint32_t seed, size_t frames, double amplitude, float* out);

}  // namespace rdacore::audio
