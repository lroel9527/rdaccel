// The drift resampler (core/audio/resampler.h): clean at ratios near 1, flat
// to 18 kHz, and smooth while the ratio changes.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/resampler.h"

using rdacore::audio::Resampler;

namespace {

// Runs a stereo sine of `hz` through the resampler at `ratio` and returns
// the left output.
std::vector<double> Resample(double hz, double ratio, size_t out_frames) {
  Resampler r(2, 16384);
  std::vector<double> out;
  std::vector<float> in(2 * 1024), o(2 * 256);
  int64_t pos = 0;
  while (out.size() < out_frames) {
    while (r.Buffered() < r.Needed(256, ratio)) {
      for (int i = 0; i < 1024; ++i) {
        const double v = 0.5 * std::sin(2 * M_PI * hz * static_cast<double>(pos + i) / 48000);
        in[2 * i] = in[2 * i + 1] = static_cast<float>(v);
      }
      pos += 1024;
      EXPECT_EQ(r.Write(in.data(), 1024), 1024u);
    }
    const size_t n = r.Read(o.data(), 256, ratio);
    EXPECT_EQ(n, 256u);
    for (size_t i = 0; i < n; ++i) out.push_back(o[2 * i]);
  }
  return out;
}

// Least-squares fit of a sine of angular frequency w (per sample) to x:
// returns the amplitude and, through `snr_db`, how much of x it explains.
double FitSine(const std::vector<double>& x, double w, double* snr_db) {
  double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
  for (size_t k = 0; k < x.size(); ++k) {
    const double s = std::sin(w * static_cast<double>(k)), c = std::cos(w * static_cast<double>(k));
    ss += s * s;
    cc += c * c;
    sc += s * c;
    xs += x[k] * s;
    xc += x[k] * c;
  }
  const double det = ss * cc - sc * sc;
  const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
  double sig = 0, err = 0;
  for (size_t k = 0; k < x.size(); ++k) {
    const double fit = a * std::sin(w * static_cast<double>(k)) + b * std::cos(w * static_cast<double>(k));
    sig += fit * fit;
    err += (x[k] - fit) * (x[k] - fit);
  }
  if (snr_db) *snr_db = 10 * std::log10(sig / std::max(err, 1e-30));
  return std::hypot(a, b);
}

}  // namespace

TEST(AudioResampler, CleanNearOne) {
  for (double ratio : {0.995, 1.0, 1.005}) {
    auto y = Resample(1000, ratio, 48'000);
    y.erase(y.begin(), y.begin() + 1000);  // past the start
    double snr = 0;
    const double amp = FitSine(y, 2 * M_PI * 1000 * ratio / 48000, &snr);
    std::printf("ratio %.3f: 1 kHz SNR %.1f dB, gain %.4f dB\n", ratio, snr, 20 * std::log10(amp / 0.5));
    EXPECT_GT(snr, 80.0) << ratio;
  }
}

TEST(AudioResampler, FlatTo18kHz) {
  for (double ratio : {0.995, 1.0, 1.005}) {
    double worst = 0;
    for (double hz = 50; hz <= 18'000; hz *= 1.25) {
      auto y = Resample(hz, ratio, 24'000);
      y.erase(y.begin(), y.begin() + 1000);
      const double amp = FitSine(y, 2 * M_PI * hz * ratio / 48000, nullptr);
      worst = std::max(worst, std::abs(20 * std::log10(amp / 0.5)));
    }
    std::printf("ratio %.3f: largest deviation to 18 kHz %.3f dB\n", ratio, worst);
    EXPECT_LT(worst, 0.1) << ratio;
  }
}

TEST(AudioResampler, AChangingRatioMakesNoJumps) {
  // The ratio swings between 0.995 and 1.005 every few calls, as the drift
  // control never would so fast; a 1 kHz sine must stay a smooth sine.
  Resampler r(2, 16384);
  std::vector<float> in(2 * 1024), o(2 * 128);
  int64_t pos = 0;
  double max_step = 0, prev = 0;
  bool have_prev = false;
  for (int call = 0; call < 2000; ++call) {
    const double ratio = 1 + 0.005 * std::sin(call * 0.05);
    while (r.Buffered() < r.Needed(128, ratio)) {
      for (int i = 0; i < 1024; ++i) {
        in[2 * i] = in[2 * i + 1] = static_cast<float>(0.5 * std::sin(2 * M_PI * 1000 * static_cast<double>(pos + i) / 48000));
      }
      pos += 1024;
      r.Write(in.data(), 1024);
    }
    const size_t n = r.Read(o.data(), 128, ratio);
    ASSERT_EQ(n, 128u);
    for (size_t i = 0; i < n; ++i) {
      ASSERT_TRUE(std::isfinite(o[2 * i]));
      if (call > 2 && have_prev) max_step = std::max(max_step, std::abs(o[2 * i] - prev));
      prev = o[2 * i];
      have_prev = true;
    }
  }
  // A 1.005 kHz sine at 0.5 moves at most 2 pi f A / fs = 0.0658 per sample.
  EXPECT_LT(max_step, 0.0658 * 1.01);
}

TEST(AudioResampler, ResetIsSilenceAndNeededIsEnough) {
  Resampler r(2, 1024);
  std::vector<float> o(2 * 64, 1.0f);
  EXPECT_EQ(r.Read(o.data(), 64, 1.0), 0u);  // nothing written yet
  std::vector<float> in(2 * 200, 0.25f);
  r.Write(in.data(), 200);
  const size_t can = 200 - Resampler::kTaps / 2;
  EXPECT_GE(r.Buffered(), r.Needed(can, 1.0));
  EXPECT_EQ(r.Read(o.data(), 64, 1.0), 64u);
  r.Reset();
  EXPECT_EQ(r.Read(o.data(), 64, 1.0), 0u);
}
