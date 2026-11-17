// The sound sender (core/audio/sender.h): the silence gate, mono when left
// and right are identical, RED, pausing, profiles. Only computed signals.

#include <gtest/gtest.h>
#include <opus.h>

#include <cmath>
#include <vector>

#include "audio/audio_packet.h"
#include "audio/sender.h"
#include "audio/test_signal.h"

using namespace rdacore::audio;

namespace {

struct Sent {
  Packet p;
  std::vector<uint8_t> bytes;
  int channels() const { return opus_packet_get_nb_channels(p.main.data()); }
  int samples() const { return opus_packet_get_nb_samples(p.main.data(), static_cast<opus_int32>(p.main.size()), 48000); }
};

// Feeds a sender 10 ms at a time and keeps what it sends.
struct Harness {
  std::vector<Sent> sent;
  Sender sender;
  int64_t pos = 0;
  explicit Harness(int profile = kProfileFull)
      : sender(
            [this](std::span<const uint8_t> b) {
              Sent s;
              s.bytes.assign(b.begin(), b.end());
              s.p = *ParsePacket(s.bytes);
              sent.push_back(std::move(s));
              return true;
            },
            profile) {}
  // `fn(first, frames, out)` makes the signal.
  template <class F>
  void Feed(double seconds, F fn) {
    const size_t total = static_cast<size_t>(seconds * 48000);
    std::vector<float> buf(480 * 2);
    for (size_t done = 0; done < total; done += 480) {
      fn(pos, 480, buf.data());
      sender.Push(buf.data(), 480, 1'000'000 + static_cast<uint64_t>(pos) * 1'000'000 / 48000);
      pos += 480;
    }
  }
  void Silence(double seconds) {
    Feed(seconds, [](int64_t, size_t n, float* o) { std::fill(o, o + 2 * n, 0.0f); });
  }
  void Tone(double seconds, double db = -20, double right_hz = 0) {
    Feed(seconds, [&](int64_t at, size_t n, float* o) { Sine(at, n, 1000, std::pow(10, db / 20), o, right_hz); });
  }
};

// Re-parse after the vector moved (spans point into `bytes`).
void Reparse(std::vector<Sent>& v) {
  for (auto& s : v) s.p = *ParsePacket(s.bytes);
}

}  // namespace

TEST(AudioGate, SilenceAndInaudibleNoiseSendNothing) {
  Harness h;
  h.Silence(1.0);
  h.Feed(1.0, [](int64_t at, size_t n, float* o) { Noise(static_cast<uint32_t>(at), n, std::pow(10, -95.0 / 20), o); });
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.sender.stats().frames_gated, 200u);
  EXPECT_EQ(h.sender.stats().frames, 0u);
  EXPECT_TRUE(h.sender.status().silent);
}

TEST(AudioGate, QuietSoundAboveTheThresholdIsSent) {
  Harness h;
  h.Tone(0.5, -70);
  Reparse(h.sent);
  EXPECT_EQ(h.sent.size(), 50u);  // every 10 ms frame
  EXPECT_FALSE(h.sender.status().silent);
}

TEST(AudioGate, A200msTailThenEnd) {
  Harness h;
  h.Tone(0.3);
  const size_t during = h.sent.size();
  h.Silence(1.0);
  Reparse(h.sent);
  // 20 silent frames still go out, the last with END; then nothing.
  ASSERT_EQ(h.sent.size(), during + 20);
  for (size_t i = 0; i + 1 < h.sent.size(); ++i) EXPECT_FALSE(h.sent[i].p.end()) << i;
  EXPECT_TRUE(h.sent.back().p.end());
  EXPECT_TRUE(h.sender.status().silent);
  EXPECT_EQ(h.sender.stats().frames_gated, 80u);
}

TEST(AudioGate, ResumingCarriesTheFrameBeforeAndTheClock) {
  Harness h;
  h.Silence(0.5);
  h.Tone(0.2);
  Reparse(h.sent);
  ASSERT_EQ(h.sent.size(), 20u);
  const Sent& first = h.sent[0];
  EXPECT_EQ(first.p.spurt, 1);
  EXPECT_EQ(first.p.ts, 24'000u);  // 0.5 s in: positions ran on through the silence
  EXPECT_TRUE(first.p.clock());
  EXPECT_EQ(first.p.time_us, 1'000'000u + 500'000u);
  ASSERT_FALSE(first.p.red.empty());  // the silent frame before, as a lead-in
  EXPECT_EQ(opus_packet_get_nb_samples(first.p.red.data(), static_cast<opus_int32>(first.p.red.size()), 48000), 480);
  // The next four have the copy and the clock too; then neither (no loss).
  for (int i = 1; i < 5; ++i) {
    EXPECT_TRUE(h.sent[i].p.clock()) << i;
    EXPECT_FALSE(h.sent[i].p.red.empty()) << i;
    // The copy is exactly the previous packet's frame.
    EXPECT_EQ(std::vector<uint8_t>(h.sent[i].p.red.begin(), h.sent[i].p.red.end()),
              std::vector<uint8_t>(h.sent[i - 1].p.main.begin(), h.sent[i - 1].p.main.end()));
  }
  for (size_t i = 5; i < h.sent.size(); ++i) {
    EXPECT_FALSE(h.sent[i].p.clock()) << i;
    EXPECT_TRUE(h.sent[i].p.red.empty()) << i;
  }
  for (size_t i = 0; i < h.sent.size(); ++i) {
    EXPECT_EQ(h.sent[i].p.seq, i);
    EXPECT_EQ(h.sent[i].p.ts, 24'000u + 480 * i);
  }
  // Another silence and sound: the next spurt.
  h.Silence(0.5);
  h.Tone(0.1);
  Reparse(h.sent);
  EXPECT_EQ(h.sent.back().p.spurt, 2);
}

TEST(AudioGate, SoundAroundTheThresholdDoesNotFlap) {
  // 50 ms of sound, 50 ms of silence, over and over: never 200 ms of
  // silence, so one spurt and no END.
  Harness h;
  h.Feed(2.0, [](int64_t at, size_t n, float* o) {
    if ((at / 2400) % 2 == 0) {
      Sine(at, n, 1000, 0.1, o);
    } else {
      std::fill(o, o + 2 * n, 0.0f);
    }
  });
  Reparse(h.sent);
  EXPECT_EQ(h.sent.size(), 200u);
  for (const auto& s : h.sent) {
    EXPECT_EQ(s.p.spurt, 1);
    EXPECT_FALSE(s.p.end());
  }
}

TEST(AudioGate, IdenticalChannelsBecomeMonoAndBackAtOnce) {
  Harness h;
  h.Tone(1.0);  // left = right
  Reparse(h.sent);
  ASSERT_EQ(h.sent.size(), 100u);
  for (size_t i = 0; i < 49; ++i) EXPECT_EQ(h.sent[i].channels(), 2) << i;  // the first 500 ms
  for (size_t i = 50; i < 100; ++i) EXPECT_EQ(h.sent[i].channels(), 1) << i;
  EXPECT_EQ(h.sender.status().channels, 1);
  EXPECT_EQ(h.sender.status().bitrate_bps, 96'000);
  // One sample that differs: that very frame is stereo again.
  h.Feed(0.01, [](int64_t at, size_t n, float* o) {
    Sine(at, n, 1000, 0.1, o);
    o[2 * 100 + 1] += 0.001f;
  });
  Reparse(h.sent);
  EXPECT_EQ(h.sent.back().channels(), 2);
  EXPECT_EQ(h.sender.status().bitrate_bps, 160'000);
}

TEST(AudioGate, PausedSendsNothingAndResumesAsANewSpurt) {
  Harness h;
  h.Tone(0.2);
  const size_t before = h.sent.size();
  h.sender.SetPaused(true);
  h.Tone(1.0);
  // One more frame, the spurt's END; then nothing.
  Reparse(h.sent);
  ASSERT_EQ(h.sent.size(), before + 1);
  EXPECT_TRUE(h.sent.back().p.end());
  EXPECT_TRUE(h.sender.status().silent);
  h.sender.SetPaused(false);
  h.Tone(0.1);
  Reparse(h.sent);
  ASSERT_GT(h.sent.size(), before + 1);
  EXPECT_EQ(h.sent[before + 1].p.spurt, 2);
  EXPECT_TRUE(h.sent[before + 1].p.clock());
  EXPECT_FALSE(h.sent[before + 1].p.red.empty());
}

TEST(AudioGate, DataSaverUses20msFrames) {
  Harness h;
  h.Tone(0.2, -20, 1500);
  h.sender.SetProfile(kProfileSaver);
  h.Tone(0.4, -20, 1500);
  Reparse(h.sent);
  EXPECT_EQ(h.sent.front().samples(), 480);
  EXPECT_EQ(h.sent.back().samples(), 960);
  EXPECT_EQ(h.sender.status().frame_ms, 20);
  EXPECT_EQ(h.sender.status().bitrate_bps, 96'000);
  // Positions stay continuous across the switch.
  for (size_t i = 1; i < h.sent.size(); ++i) {
    EXPECT_EQ(h.sent[i].p.ts, h.sent[i - 1].p.ts + static_cast<uint32_t>(h.sent[i - 1].samples())) << i;
  }
}

TEST(AudioGate, RedWhileThePathLosesPackets) {
  Harness h;
  h.Tone(1.0, -20, 1500);
  h.sender.SetPath(0.02, 0);
  h.Tone(1.5, -20, 1500);  // the path is looked at once a second
  Reparse(h.sent);
  EXPECT_FALSE(h.sent.back().p.red.empty());
  const auto red_bytes = h.sender.stats().red_bytes;
  EXPECT_GT(red_bytes, 0u);
  // Off again only after 30 s below 0.3%.
  h.sender.SetPath(0.001, 0);
  h.Tone(10, -20, 1500);
  Reparse(h.sent);
  EXPECT_FALSE(h.sent.back().p.red.empty());
  h.Tone(25, -20, 1500);
  Reparse(h.sent);
  EXPECT_TRUE(h.sent.back().p.red.empty());
}

TEST(AudioGate, AGapInTheInputIsSilence) {
  Harness h;
  h.Tone(0.1);
  // The source stops calling back for 2 s, then sound again.
  h.pos += 2 * 48000;
  h.Tone(0.1);
  Reparse(h.sent);
  ASSERT_GE(h.sent.size(), 30u);
  // The tail of 200 ms of silence went out with END; then the next spurt,
  // at the position the time says.
  bool ended = false;
  for (const auto& s : h.sent) ended |= s.p.end();
  EXPECT_TRUE(ended);
  const Sent& last = h.sent.back();
  EXPECT_EQ(last.p.spurt, 2);
  EXPECT_EQ(last.p.ts, static_cast<uint32_t>(h.pos - 480));
}

TEST(AudioGate, ASlowPathLowersTheBitrateAndItComesBackInSteps) {
  Harness h;
  h.Tone(1.0, -20, 1500);
  EXPECT_EQ(h.sender.status().bitrate_bps, 160'000);
  // 400 kbps: a quarter is 100 kbps, of which the headers of 100 packets a
  // second take 52: 48 kbps for the sound.
  h.sender.SetPath(0, 400'000, /*congested=*/true);
  h.Tone(1.5, -20, 1500);
  EXPECT_EQ(h.sender.status().bitrate_bps, 48'000);
  // Slower still: never below 32 kbps.
  h.sender.SetPath(0, 250'000, /*congested=*/true);
  h.Tone(1.5, -20, 1500);
  EXPECT_EQ(h.sender.status().bitrate_bps, 32'000);
  // The path is no longer full: one step every 5 s, 32 -> 48 -> 64 -> 96 -> 160.
  h.sender.SetPath(0, 10'000'000);
  std::vector<int> seen;
  for (int i = 0; i < 30; ++i) {
    h.Tone(1.0, -20, 1500);
    const int b = h.sender.status().bitrate_bps;
    if (seen.empty() || seen.back() != b) seen.push_back(b);
  }
  EXPECT_EQ(seen, (std::vector<int>{32'000, 48'000, 64'000, 96'000, 160'000}));
  // No estimate (a pipe, or not yet trusted): as the profile says, nothing changes.
  Harness unmeasured;
  unmeasured.sender.SetPath(0, 0);
  unmeasured.Tone(2.0, -20, 1500);
  EXPECT_EQ(unmeasured.sender.status().bitrate_bps, 160'000);
}

TEST(AudioGate, DataSaverOnASlowPath) {
  // 20 ms frames: half the packets, so less of the quarter goes to headers.
  Harness h(kProfileSaver);
  h.Tone(1.0, -20, 1500);
  h.sender.SetPath(0, 400'000, /*congested=*/true);  // 100 - 26 = 74 kbps
  h.Tone(1.5, -20, 1500);
  EXPECT_EQ(h.sender.status().bitrate_bps, 74'000);
  EXPECT_EQ(h.sender.status().frame_ms, 20);
}

TEST(AudioGate, AShortGapStillMovesThePositionsOn) {
  // A source that missed two 10 ms callbacks: 20 ms of silence, and the
  // sample positions follow the capture time (not the samples alone).
  Harness h;
  h.Tone(0.2);
  h.pos += 960;  // 20 ms the source did not deliver
  h.Tone(0.1);
  Reparse(h.sent);
  EXPECT_EQ(h.sent.back().p.ts, static_cast<uint32_t>(h.pos - 480));
  // Timestamps that jitter by less than a chunk punch no holes.
  Harness j;
  std::vector<float> buf(480 * 2);
  for (int64_t k = 0; k < 50; ++k) {
    Sine(k * 480, 480, 1000, 0.1, buf.data());
    const int64_t jitter = (k % 3 == 1) ? 4'000 : (k % 3 == 2 ? -3'000 : 0);
    j.sender.Push(buf.data(), 480, static_cast<uint64_t>(1'000'000 + k * 10'000 + jitter));
  }
  Reparse(j.sent);
  EXPECT_EQ(j.sent.back().p.ts, 49u * 480);
}

TEST(AudioGate, TheSoundAloneDoesNotKeepItselfDown) {
  // A still screen with music: the sound is all the traffic, so the estimate
  // measures about the sound (~200 kbps on the wire). A round trip that
  // jumps for a while (a queue elsewhere) brings the rate down; once it is
  // over the rate comes back to the profile's although the estimate stays
  // that low.
  Harness h;
  h.Tone(1.0, -20, 1500);
  h.sender.SetPath(0, 200'000, /*congested=*/true);
  h.Tone(2.0, -20, 1500);
  EXPECT_EQ(h.sender.status().bitrate_bps, 32'000);
  h.sender.SetPath(0, 200'000, /*congested=*/false);
  h.Tone(25.0, -20, 1500);
  EXPECT_EQ(h.sender.status().bitrate_bps, 160'000);
  // A low estimate on a path that is not full lowers nothing.
  Harness quiet;
  quiet.sender.SetPath(0, 200'000, /*congested=*/false);
  quiet.Tone(3.0, -20, 1500);
  EXPECT_EQ(quiet.sender.status().bitrate_bps, 160'000);
}
