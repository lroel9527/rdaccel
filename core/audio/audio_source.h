#pragma once

// Where the host's sound comes from: the computer's own output (macOS Process
// Tap, platform/mac/audio_tap.mm), a computed test signal (test_signal.h),
// or the application (rda_host_submit_audio, which needs no Source).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rdacore::audio {

// Everything inside the library is 48 kHz, stereo, interleaved float.
inline constexpr int kSampleRate = 48000;
inline constexpr int kChannels = 2;

struct SourceConfig {
  // The computer stays silent while its sound is captured (macOS:
  // CATapMutedWhenTapped), only once the capture is known to have the
  // permission (without it the capture hears nothing, and both sides would
  // be silent).
  bool mute_local = false;
  // Applications (bundle identifiers) left out of the capture, besides this
  // process, which always is.
  std::vector<std::string> exclude_bundle_ids;
};

enum class SourceState { kStarting, kRunning, kNoPermission, kFailed };

class Source {
 public:
  // 48 kHz, stereo, interleaved float; time_us is when the first sample was
  // captured (capture::MonotonicMicros' clock). On the source's own sound
  // thread, never on a real-time audio thread.
  using PcmFn = std::function<void(const float* interleaved, size_t frames, uint64_t time_us)>;
  // State changes (kFailed and kNoPermission with why). Any source thread.
  using StateFn = std::function<void(SourceState, const std::string& message)>;
  // Whether the capture mutes this computer right now.
  virtual bool muting_local() const { return false; }

  virtual ~Source() = default;
  virtual bool Start(const SourceConfig& config, PcmFn pcm, StateFn state) = 0;
  // No callback runs after it returns.
  virtual void Stop() = 0;
};

// macOS 14.2: everything this computer plays (Core Audio Process Tap).
// Null where there is none (Linux).
std::unique_ptr<Source> CreateSystemSource();
// A computed test signal in real time (test_signal.h); touches no device.
std::unique_ptr<Source> CreateTestSource();

}  // namespace rdacore::audio
