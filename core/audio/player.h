#pragma once

// Plays the viewer's sound (rda_audio_player): the default output device
// (macOS: platform/mac/audio_player.mm; it follows the default device when
// that changes), or a null device that takes 5 ms every 5 ms on a thread of
// its own and throws it away (tests, command-line tools). Both pull through
// the same RenderFn. RDACCEL_AUDIO_NULL_OUTPUT=1 in the environment forces
// the null device whatever is asked for: nothing a test does can make a
// sound.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace rdacore::audio {

// Fills exactly `frames` stereo frames; returns how many were sound.
// play_time_us: when the first reaches the speaker (0 = now).
using RenderFn = std::function<size_t(float* out, size_t frames, uint64_t play_time_us)>;

class Player {
 public:
  virtual ~Player() = default;
  // 1: the default output is muted or at volume 0; 0: audible; -1: not known.
  // Read-only queries; nothing is ever set.
  virtual int OutputMuted() const { return -1; }
  // Frames rendered so far.
  virtual uint64_t frames() const = 0;
};

// The environment forces the null device (RDACCEL_AUDIO_NULL_OUTPUT=1).
bool NullOutputForced();
std::unique_ptr<Player> CreateNullPlayer(RenderFn render);
// The default output device; null (with `error`) where there is none to
// open (Linux so far).
std::unique_ptr<Player> CreateDevicePlayer(RenderFn render, std::string* error);
// How many device players this process has opened (tests check it stays 0).
uint64_t DevicePlayersOpened();

}  // namespace rdacore::audio
