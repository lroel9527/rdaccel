// The sound receiver (core/audio/receiver.h) against a simulated network on
// a fake clock: the target delay follows the jitter, losses are concealed
// (or repaired from RED), a stall is caught up once, clock drift is followed
// without dropping or inserting anything, and a spurt's start is whole.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "audio/receiver.h"
#include "audio/sender.h"
#include "audio/test_signal.h"
#include "audio_test_util.h"

using namespace rdacore::audio;
using namespace audio_test;

namespace {

// A sender and a receiver on one fake clock (the receiver's, in us). The
// sender's sound card runs `ppm` faster; packets take `delay_ms` plus a
// uniform 0..jitter_ms, some are lost or duplicated; the receiver plays
// 5 ms (240 frames) at a time.
struct Sim {
  double ppm = 0;
  int delay_ms = 20, jitter_ms = 0;
  double loss = 0, duplicate = 0;
  uint64_t stall_from = 0, stall_until = 0;  // nothing arrives in between (it all comes at the end)
  std::mt19937 rng{1};
  Receiver rx;
  std::multimap<uint64_t, std::vector<uint8_t>> in_flight;
  uint64_t now = 1'000'000;
  uint64_t send_time = 0;  // when the frame being pushed is complete (receiver clock)
  int64_t sent = 0;  // frames the sender has produced
  std::vector<float> in, out;  // all of it
  std::vector<float> chunk = std::vector<float>(480 * 2);
  Sender tx;
  explicit Sim(int profile = kProfileFull)
      : tx([this](std::span<const uint8_t> b) { Send(b); return true; }, profile) {}

  void Send(std::span<const uint8_t> b) {
    std::uniform_real_distribution<double> u(0, 1);
    if (u(rng) < loss) return;
    const int copies = u(rng) < duplicate ? 2 : 1;
    for (int c = 0; c < copies; ++c) {
      uint64_t at = send_time + static_cast<uint64_t>(delay_ms * 1000 + u(rng) * jitter_ms * 1000);
      if (at >= stall_from && at < stall_until) at = stall_until;
      in_flight.emplace(at, std::vector<uint8_t>(b.begin(), b.end()));
    }
  }

  // Runs `seconds` of receiver time with the sender producing `fn`.
  template <class F>
  void Run(double seconds, F fn) {
    const uint64_t end = now + static_cast<uint64_t>(seconds * 1e6);
    std::vector<float> buf(240 * 2);
    while (now < end) {
      now += 5'000;
      // The sender's clock: (1 + ppm) as fast.
      const double sender_us = static_cast<double>(now) * (1 + ppm * 1e-6);
      while (static_cast<double>(sent) * 1e6 / 48000 < sender_us - 1e6) {
        fn(sent, 480, chunk.data());
        in.insert(in.end(), chunk.begin(), chunk.end());
        // A frame goes out once its last sample is there.
        send_time = 1'000'000 + static_cast<uint64_t>(static_cast<double>(sent + 480) * 1e6 / 48000 / (1 + ppm * 1e-6));
        tx.Push(chunk.data(), 480, static_cast<uint64_t>(sent) * 1'000'000 / 48000 + 1);
        sent += 480;
      }
      while (!in_flight.empty() && in_flight.begin()->first <= now) {
        rx.OnPacket(in_flight.begin()->second, in_flight.begin()->first);
        in_flight.erase(in_flight.begin());
      }
      rx.Read(buf.data(), 240, now, now);
      out.insert(out.end(), buf.begin(), buf.end());
    }
  }
  void Tone(double seconds) {
    Run(seconds, [](int64_t at, size_t n, float* o) { Sine(at, n, 1000, 0.1, o, 1500); });
  }
};

}  // namespace

TEST(AudioJitter, TargetFollowsTheJitter) {
  for (int jitter : {0, 5, 20, 40}) {
    Sim s;
    s.jitter_ms = jitter;
    s.rng.seed(static_cast<uint32_t>(jitter + 3));
    s.Tone(2.0);  // the first packets set the target
    const auto settle = s.rx.stats();
    s.Tone(15.0);
    const auto st = s.rx.stats();
    const double expect = std::clamp(jitter + 5.0, 20.0, 60.0);
    std::printf("jitter %2d ms: target %d ms (expected about %.0f), buffer %d ms, underruns %llu, concealed %llu\n",
                jitter, st.target_ms, expect, st.buffer_ms, static_cast<unsigned long long>(st.underruns),
                static_cast<unsigned long long>(st.lost));
    EXPECT_NEAR(st.target_ms, expect, 3.0) << jitter;
    EXPECT_EQ(st.underruns, settle.underruns) << jitter;
    EXPECT_EQ(st.lost, settle.lost) << jitter;
  }
}

TEST(AudioJitter, LossIsConcealedAndRedRepairsIt) {
  {
    Sim s;
    s.loss = 0.01;
    s.jitter_ms = 5;
    s.Tone(20);
    const auto st = s.rx.stats();
    std::printf("1%% loss: %llu frames concealed, %llu underruns\n", static_cast<unsigned long long>(st.lost),
                static_cast<unsigned long long>(st.underruns));
    EXPECT_GT(st.lost, 5u);
    EXPECT_EQ(st.underruns, 0u);
  }
  {
    Sim s;
    s.loss = 0.01;
    s.jitter_ms = 5;
    s.tx.SetPath(0.01, 0);  // the host sees the loss: RED on every packet
    s.Tone(20);
    const auto st = s.rx.stats();
    std::printf("1%% loss with RED: %llu frames concealed\n", static_cast<unsigned long long>(st.lost));
    EXPECT_EQ(st.lost, 0u);
    EXPECT_EQ(st.underruns, 0u);
  }
}

TEST(AudioJitter, ReorderedAndDuplicatedPacketsAreSortedOut) {
  Sim s;
  s.jitter_ms = 30;  // reorders often (10 ms apart)
  s.duplicate = 0.1;
  s.Tone(15);
  const auto st = s.rx.stats();
  EXPECT_EQ(st.underruns, 0u);
  EXPECT_EQ(st.lost, 0u);
  EXPECT_GT(st.packets, 1500u);
}

TEST(AudioJitter, AStallIsCaughtUpOnceWithACrossfade) {
  Sim s;
  s.jitter_ms = 5;
  s.Tone(5);
  s.stall_from = s.now;
  s.stall_until = s.now + 300'000;
  s.Tone(20);
  const auto st = s.rx.stats();
  std::printf("after a 300 ms stall: %llu catch-up, %llu underrun, buffer %d ms, target %d ms\n",
              static_cast<unsigned long long>(st.catchups), static_cast<unsigned long long>(st.underruns), st.buffer_ms,
              st.target_ms);
  EXPECT_EQ(st.catchups, 1u);
  EXPECT_LE(st.underruns, 1u);  // the stall itself ran it dry
  EXPECT_LT(st.buffer_ms, st.target_ms + 15);
}

TEST(AudioJitter, ClockDriftIsFollowedWithoutDroppingOrRepeating) {
  for (double ppm : {200.0, -200.0}) {
    Sim s;
    s.ppm = ppm;
    s.jitter_ms = 2;
    s.Tone(60);  // settles
    double worst = 0, sum = 0;
    int n = 0;
    // 9 more minutes, the level averaged over each second.
    for (int sec = 0; sec < 540; ++sec) {
      double level = 0;
      for (int i = 0; i < 10; ++i) {
        s.Tone(0.1);
        level += s.rx.stats().buffer_ms;
      }
      level /= 10;
      worst = std::max(worst, std::abs(level - s.rx.stats().target_ms));
      sum += s.rx.stats().drift_ppm;
      ++n;
    }
    const auto st = s.rx.stats();
    std::printf("%+.0f ppm: drift control at %.0f ppm on average, level off target by at most %.1f ms\n", ppm,
                sum / n, worst);
    EXPECT_LT(worst, 5.0) << ppm;
    EXPECT_EQ(st.underruns, 0u) << ppm;
    EXPECT_EQ(st.catchups, 0u) << ppm;
    EXPECT_EQ(st.lost, 0u) << ppm;
    EXPECT_NEAR(sum / n, ppm, 60) << ppm;
  }
}

TEST(AudioJitter, ASpurtStartsWhole) {
  for (int profile : {kProfileFull, kProfileSaver}) {
    Sim s(profile);
    s.jitter_ms = 10;
    // Silence, then sound that starts in the middle of a frame (tones that
    // do not repeat, so the alignment below is unambiguous).
    const int64_t onset = 3 * 48000 + 4321;
    s.Run(4.0, [&](int64_t at, size_t n, float* o) {
      Multitone(at, n, 0.4, o);
      for (size_t i = 0; i < n; ++i) {
        if (at + static_cast<int64_t>(i) < onset) o[2 * i] = o[2 * i + 1] = 0;
      }
    });
    // Align output to input around the onset.
    const auto x = Channel(s.in, 0, onset, 2400);
    const size_t out_frames = s.out.size() / 2;
    // The output runs behind the input by the path and the buffer: find
    // where the onset came out.
    size_t first_out = 0;
    for (size_t i = 0; i < out_frames; ++i) {
      if (std::abs(s.out[2 * i]) > 1e-3) {
        first_out = i;
        break;
      }
    }
    ASSERT_GT(first_out, 0u);
    const size_t from = first_out > 2000 ? first_out - 2000 : 0;
    const auto y = Channel(s.out, 0, from, 2400 + 4000);
    const int lag = BestLag(x, y, 4000);
    double xy = 0, xx = 0, yy = 0;
    for (size_t i = 0; i < x.size(); ++i) {
      xy += x[i] * y[i + lag];
      xx += x[i] * x[i];
      yy += y[i + lag] * y[i + lag];
    }
    const double corr = xy / std::sqrt(xx * yy);
    // And the delay from capture to output is what the path and buffer make.
    const double delay_ms = (static_cast<double>(from + lag) - static_cast<double>(onset)) / 48.0;
    std::printf("profile %d: correlation from the first audible sample %.3f, delay %.0f ms, target %d ms\n", profile,
                corr, delay_ms, s.rx.stats().target_ms);
    EXPECT_GT(corr, 0.9) << profile;
    // Nothing before the onset came out (no early noise from the lead-in).
    double before = 0;
    for (size_t i = 0; i + 200 < from + lag; ++i) before = std::max(before, static_cast<double>(std::abs(s.out[2 * i])));
    EXPECT_LT(before, 1e-3) << profile;
  }
}
