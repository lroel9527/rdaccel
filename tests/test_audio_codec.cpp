// Opus as the sender uses it (core/audio/sender.h): what comes out is close
// to what went in, the attack after silence is whole, the bitrate is near
// its target, it is fast, and changes of frame length and channels are
// seamless. A floor against gross mistakes, not a measure of sound quality:
// perceptual coding does not keep the waveform.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/sender.h"
#include "audio/test_signal.h"
#include "audio_test_util.h"

using namespace rdacore::audio;
using namespace audio_test;

namespace {

struct Encoded {
  std::vector<std::vector<uint8_t>> packets;
  std::vector<float> in;  // what went in
  uint64_t bytes = 0;
};

// Sends `seconds` of the signal `fn` through a sender of `profile`; the
// signal may change the profile or channels as it goes (`each` before every
// 10 ms chunk).
template <class F, class G>
Encoded Encode(int profile, double seconds, F fn, G each) {
  Encoded r;
  Sender s(
      [&](std::span<const uint8_t> b) {
        r.packets.emplace_back(b.begin(), b.end());
        r.bytes += b.size();
        return true;
      },
      profile);
  const size_t total = static_cast<size_t>(seconds * 48000);
  r.in.resize(total * 2);
  for (size_t at = 0; at < total; at += 480) {
    each(s, static_cast<int64_t>(at));
    fn(static_cast<int64_t>(at), 480, r.in.data() + 2 * at);
    s.Push(r.in.data() + 2 * at, 480, static_cast<uint64_t>(at) * 1'000'000 / 48000 + 1);
  }
  return r;
}

template <class F>
Encoded Encode(int profile, double seconds, F fn) {
  return Encode(profile, seconds, fn, [](Sender&, int64_t) {});
}

void ExpectClose(int profile) {
  const Encoded r = Encode(profile, 3.0, [](int64_t at, size_t n, float* o) { Multitone(at, n, 0.5, o); });
  const auto out = DecodeAll(r.packets, 0, r.in.size() / 2);
  for (int ch = 0; ch < 2; ++ch) {
    // Past the first half second (the encoder settles), 2^16 samples.
    const auto x = Channel(r.in, ch, 24'000, 65'536 + 2'000);
    const auto y = Channel(out, ch, 24'000, 65'536 + 2'000);
    const int lag = BestLag(x, y, 600);
    EXPECT_LT(lag, 300) << "codec delay";
    const std::vector<double> ys(y.begin() + lag, y.end());
    const auto ex = BandEnergiesDb(x), ey = BandEnergiesDb(ys);
    double max_e = -1e9;
    for (double e : ex) max_e = std::max(max_e, e);
    double diff = 0;
    int bands = 0;
    for (size_t b = 0; b < ex.size(); ++b) {
      if (ex[b] < max_e - 40) continue;  // no tone in this band
      diff += std::abs(ex[b] - ey[b]);
      ++bands;
    }
    ASSERT_GT(bands, 8);
    EXPECT_LT(diff / bands, 1.0) << "profile " << profile << " channel " << ch;
    const double snr = SnrDb(x, y, lag);
    std::printf("profile %d channel %d: band difference %.2f dB over %d bands, SNR %.1f dB, delay %d\n", profile, ch,
                diff / bands, bands, snr, lag);
    EXPECT_GT(snr, 15.0);
  }
}

}  // namespace

TEST(AudioCodec, MultitoneComesBackCloseInFull) { ExpectClose(kProfileFull); }
TEST(AudioCodec, MultitoneComesBackCloseInDataSaver) { ExpectClose(kProfileSaver); }

TEST(AudioCodec, TheAttackAfterSilenceIsWhole) {
  for (int profile : {kProfileFull, kProfileSaver}) {
    // 1 s of silence, then a 1 kHz burst at -20 dBFS that starts in the
    // middle of a frame.
    const int64_t onset = 48'000 + 333;
    const Encoded r = Encode(profile, 1.5, [&](int64_t at, size_t n, float* o) {
      Sine(at, n, 1000, 0.1, o);
      for (size_t i = 0; i < n; ++i) {
        if (at + static_cast<int64_t>(i) < onset) o[2 * i] = o[2 * i + 1] = 0;
      }
    });
    const auto out = DecodeAll(r.packets, 0, r.in.size() / 2);
    const auto x = Channel(r.in, 0), y = Channel(out, 0);
    // (A sine repeats every 48 samples: the search stays below the next
    // period past the codec's 2.5 ms.)
    const int lag = BestLag(Channel(r.in, 0, onset, 4800), Channel(out, 0, onset, 4800 + 600), 150);
    // Nothing above -60 dBFS more than 5 ms before the onset.
    double before = 0;
    for (int64_t i = 0; i + lag < onset - 240; ++i) before = std::max(before, std::abs(y[i + lag]));
    EXPECT_LT(20 * std::log10(before + 1e-12), -60.0) << "profile " << profile;
    // The first 5 ms after it carry the energy they should.
    double ex = 0, ey = 0;
    for (int64_t i = onset; i < onset + 240; ++i) {
      ex += x[i] * x[i];
      ey += y[i + lag] * y[i + lag];
    }
    const double db = 10 * std::log10(ey / ex);
    std::printf("profile %d: attack energy %+.2f dB, before onset %.1f dBFS, delay %d\n", profile, db,
                20 * std::log10(before + 1e-12), lag);
    EXPECT_LT(std::abs(db), 1.0) << "profile " << profile;
  }
}

TEST(AudioCodec, BitrateIsNearTheTarget) {
  for (int profile : {kProfileFull, kProfileSaver}) {
    // Busy, stereo content: tones over noise.
    const Encoded r = Encode(profile, 10.0, [](int64_t at, size_t n, float* o) {
      Multitone(at, n, 0.3, o);
      std::vector<float> noise(2 * n);
      Noise(static_cast<uint32_t>(at), n, 0.05, noise.data());
      for (size_t i = 0; i < 2 * n; ++i) o[i] += noise[i];
    });
    // The Opus bytes alone (the packet header is not the encoder's).
    uint64_t opus = 0;
    for (const auto& p : r.packets) opus += ParsePacket(p)->main.size();
    const double kbps = static_cast<double>(opus) * 8 / 10.0 / 1000;
    const double target = ProfileBitrate(profile, 2) / 1000.0;
    std::printf("profile %d: %.0f kbps of Opus for a %.0f kbps target, %.0f kbps with headers\n", profile, kbps, target,
                static_cast<double>(r.bytes) * 8 / 10.0 / 1000);
    EXPECT_GT(kbps, target * 0.75);
    EXPECT_LT(kbps, target * 1.25);
  }
}

TEST(AudioCodec, EncodesFarFasterThanRealTime) {
  for (int profile : {kProfileFull, kProfileSaver}) {
    std::vector<float> pcm(48'000 * 2 * 10);
    Multitone(0, 480'000, 0.3, pcm.data());
    size_t packets = 0;
    Sender s([&](std::span<const uint8_t>) { return ++packets > 0; }, profile);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < 480'000; at += 480) s.Push(pcm.data() + 2 * at, 480, at * 1'000'000 / 48'000 + 1);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
#if defined(__aarch64__) || defined(__arm64__)
    const char* arch = "arm64";
#else
    const char* arch = "x86_64";
#endif
    std::printf("profile %d (%s): 10 s of sound encoded in %.0f ms, %.0fx real time\n", profile, arch, secs * 1000,
                10 / secs);
    EXPECT_GT(10 / secs, 50.0);
    EXPECT_GT(packets, 400u);
  }
}

TEST(AudioCodec, SwitchingFramesAndChannelsIsSeamless) {
  // A 440 Hz tone, left = right for a second (mono from 0.5 s), then a
  // right channel that differs (stereo), with data saver on in between.
  const int64_t to_saver = 40'000, to_stereo = 60'000, to_full = 80'000;
  const Encoded r = Encode(
      kProfileFull, 2.0,
      [&](int64_t at, size_t n, float* o) {
        Sine(at, n, 440, 0.2, o);
        for (size_t i = 0; i < n; ++i) {
          if (at + static_cast<int64_t>(i) >= to_stereo) o[2 * i + 1] *= 0.5f;
        }
      },
      [&](Sender& s, int64_t at) {
        if (at == to_saver) s.SetProfile(kProfileSaver);
        if (at == to_full) s.SetProfile(kProfileFull);
      });
  const auto out = DecodeAll(r.packets, 0, r.in.size() / 2);
  // Sample-to-sample steps around each switch against those of the steady
  // tone (a 440 Hz sine at 0.2 changes by at most ~0.0115 per sample).
  auto max_step = [&](int ch, int64_t from, int64_t to) {
    double m = 0;
    for (int64_t i = from + 1; i < to; ++i) m = std::max(m, static_cast<double>(std::abs(out[2 * i + ch] - out[2 * (i - 1) + ch])));
    return m;
  };
  for (int ch = 0; ch < 2; ++ch) {
    const double steady = max_step(ch, 20'000, 30'000);
    for (int64_t at : {int64_t{24'000}, to_saver, to_stereo, to_full}) {
      // The switch shows in the output a codec delay later.
      const double around = max_step(ch, at - 96, at + 96 + 300);
      EXPECT_LT(around, steady * 1.5 + 0.002) << "channel " << ch << " at " << at;
    }
  }
}
