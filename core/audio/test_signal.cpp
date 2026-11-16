#include "audio/test_signal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "audio/audio_source.h"
#include "capture/capturer.h"

namespace rdacore::audio {
namespace {

double Db(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

void Sine(int64_t first, size_t frames, double hz, double amplitude, float* out, double right_hz) {
  const double r = right_hz > 0 ? right_hz : hz;
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(first + static_cast<int64_t>(i)) / kSampleRate;
    out[2 * i] = static_cast<float>(amplitude * std::sin(2 * M_PI * hz * t));
    out[2 * i + 1] = static_cast<float>(amplitude * std::sin(2 * M_PI * r * t));
  }
}

void Multitone(int64_t first, size_t frames, double amplitude, float* out) {
  static const double kHz[12] = {110, 277, 523, 931, 1471, 2311, 3307, 4789, 6563, 8821, 11113, 14983};
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(first + static_cast<int64_t>(i)) / kSampleRate;
    double l = 0, r = 0;
    for (int k = 0; k < 12; ++k) {
      l += std::sin(2 * M_PI * kHz[k] * t + 0.7 * k);
      r += std::sin(2 * M_PI * kHz[k] * t + 1.3 * k);
    }
    out[2 * i] = static_cast<float>(amplitude * l / 12);
    out[2 * i + 1] = static_cast<float>(amplitude * r / 12);
  }
}

void Sweep(int64_t first, size_t frames, size_t length, double amplitude, float* out) {
  const double f0 = 20, f1 = 20'000, dur = static_cast<double>(length) / kSampleRate;
  const double k = std::log(f1 / f0);
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>((first + static_cast<int64_t>(i)) % static_cast<int64_t>(length)) / kSampleRate;
    const double phase = 2 * M_PI * f0 * dur / k * (std::exp(t / dur * k) - 1);
    out[2 * i] = out[2 * i + 1] = static_cast<float>(amplitude * std::sin(phase));
  }
}

void Noise(uint32_t seed, size_t frames, double amplitude, float* out) {
  uint32_t s = seed * 2654435761u + 1;
  for (size_t i = 0; i < 2 * frames; ++i) {
    s = s * 1664525u + 1013904223u;
    const double u = static_cast<double>(s >> 8) / static_cast<double>(1u << 24) * 2 - 1;  // -1..1
    out[i] = static_cast<float>(amplitude * u);
  }
}

void TestLoop(int64_t first, size_t frames, float* out) {
  for (size_t i = 0; i < frames;) {
    const int64_t pos = (first + static_cast<int64_t>(i)) % kTestLoopFrames;
    // Up to the next boundary of the loop's parts.
    static const int64_t kEdges[] = {14'400, 72'000, 120'000, 124'800, 144'000, 192'000};
    int64_t edge = kTestLoopFrames;
    for (int64_t e : kEdges) {
      if (pos < e) {
        edge = e;
        break;
      }
    }
    const size_t n = std::min<size_t>(frames - i, static_cast<size_t>(edge - pos));
    float* o = out + 2 * i;
    const int64_t at = first + static_cast<int64_t>(i);
    if (pos < 14'400) {
      Sine(at, n, 1000, Db(-20), o);
    } else if (pos < 72'000) {
      std::fill(o, o + 2 * n, 0.0f);
    } else if (pos < 120'000) {
      Sine(at, n, 440, Db(-20), o, 660);
    } else if (pos < 124'800) {
      Noise(static_cast<uint32_t>(at), n, Db(-95), o);
    } else if (pos < 144'000) {
      Sine(at, n, 1000, Db(-70), o);
    } else {
      std::fill(o, o + 2 * n, 0.0f);
    }
    i += n;
  }
}

namespace {

// The test loop in real time, 10 ms at a time, on its own thread.
class TestSource final : public Source {
 public:
  ~TestSource() override { Stop(); }
  bool Start(const SourceConfig&, PcmFn pcm, StateFn state) override {
    Stop();
    running_ = true;
    thread_ = std::thread([this, pcm = std::move(pcm), state = std::move(state)] {
      if (state) state(SourceState::kRunning, "");
      constexpr size_t kChunk = 480;
      std::vector<float> buf(kChunk * 2);
      const uint64_t t0 = capture::MonotonicMicros();
      int64_t pos = 0;
      auto next = std::chrono::steady_clock::now();
      while (running_) {
        TestLoop(pos, kChunk, buf.data());
        pcm(buf.data(), kChunk, t0 + static_cast<uint64_t>(pos) * 1'000'000 / kSampleRate);
        pos += kChunk;
        next += std::chrono::microseconds(10'000);
        std::this_thread::sleep_until(next);
      }
    });
    return true;
  }
  void Stop() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<Source> CreateTestSource() { return std::make_unique<TestSource>(); }

}  // namespace rdacore::audio
