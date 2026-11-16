#pragma once

// Helpers for the sound tests: decoding what a sender sent, spectra,
// alignment. Only computed signals; nothing is played or recorded.

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "audio/audio_packet.h"

namespace audio_test {

using rdacore::audio::Packet;
using rdacore::audio::ParsePacket;

// Decodes packets (in order, none lost) into one stereo timeline starting at
// sample position `origin`; a new spurt resets the decoder and its lead-in
// (RED) frame is decoded first, as the receiver does.
inline std::vector<float> DecodeAll(const std::vector<std::vector<uint8_t>>& packets, int64_t origin,
                                    size_t total_frames) {
  std::vector<float> out(total_frames * 2, 0.0f);
  int err = 0;
  OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
  std::vector<float> pcm(5760 * 2);
  int spurt = -1;
  auto put = [&](int64_t ts, const float* p, int n) {
    for (int i = 0; i < n; ++i) {
      const int64_t at = ts - origin + i;
      if (at < 0 || at >= static_cast<int64_t>(total_frames)) continue;
      out[2 * at] = p[2 * i];
      out[2 * at + 1] = p[2 * i + 1];
    }
  };
  for (const auto& b : packets) {
    const auto p = ParsePacket(b);
    if (!p) continue;
    if (p->spurt != spurt) {
      spurt = p->spurt;
      opus_decoder_ctl(dec, OPUS_RESET_STATE);
      if (!p->red.empty()) {
        const int n = opus_decode_float(dec, p->red.data(), static_cast<opus_int32>(p->red.size()), pcm.data(), 5760, 0);
        if (n > 0) put(static_cast<int64_t>(p->ts) - n, pcm.data(), n);
      }
    }
    const int n = opus_decode_float(dec, p->main.data(), static_cast<opus_int32>(p->main.size()), pcm.data(), 5760, 0);
    if (n > 0) put(p->ts, pcm.data(), n);
  }
  opus_decoder_destroy(dec);
  return out;
}

// One channel of a stereo buffer.
inline std::vector<double> Channel(const std::vector<float>& st, int ch, size_t from = 0, size_t n = SIZE_MAX) {
  const size_t frames = st.size() / 2;
  n = std::min(n, frames - std::min(frames, from));
  std::vector<double> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = st[2 * (from + i) + ch];
  return v;
}

// The lag (0..max_lag) at which `y` best matches `x` (y[i + lag] ~ x[i]).
inline int BestLag(const std::vector<double>& x, const std::vector<double>& y, int max_lag) {
  int best = 0;
  double best_c = -1e300;
  for (int lag = 0; lag <= max_lag; ++lag) {
    double c = 0;
    for (size_t i = 0; i + lag < y.size() && i < x.size(); ++i) c += x[i] * y[i + lag];
    if (c > best_c) {
      best_c = c;
      best = lag;
    }
  }
  return best;
}

// Signal to noise ratio of y (shifted by lag) against x, in dB.
inline double SnrDb(const std::vector<double>& x, const std::vector<double>& y, int lag) {
  double s = 0, e = 0;
  for (size_t i = 0; i + lag < y.size() && i < x.size(); ++i) {
    s += x[i] * x[i];
    const double d = x[i] - y[i + lag];
    e += d * d;
  }
  return 10 * std::log10(s / std::max(e, 1e-30));
}

inline void Fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * M_PI / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1);
      for (size_t k = 0; k < len / 2; ++k) {
        const auto u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Energy in `bands` logarithmic bands from 50 Hz to 20 kHz (dB), of `x`
// (Hann-windowed, the first power of two samples of it).
inline std::vector<double> BandEnergiesDb(const std::vector<double>& x, int bands = 20) {
  size_t n = 1;
  while (n * 2 <= x.size()) n *= 2;
  std::vector<std::complex<double>> a(n);
  for (size_t i = 0; i < n; ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos(2 * M_PI * static_cast<double>(i) / static_cast<double>(n)));
  Fft(a);
  std::vector<double> e(bands, 0);
  for (size_t k = 1; k < n / 2; ++k) {
    const double hz = static_cast<double>(k) * 48000.0 / static_cast<double>(n);
    if (hz < 50 || hz >= 20000) continue;
    const int b = static_cast<int>(std::log(hz / 50) / std::log(20000.0 / 50) * bands);
    e[std::clamp(b, 0, bands - 1)] += std::norm(a[k]);
  }
  for (auto& v : e) v = 10 * std::log10(v + 1e-30);
  return e;
}

// The amplitude of `hz` in x (a single DFT bin, Hann window), linear.
inline double Tone(const std::vector<double>& x, double hz) {
  std::complex<double> acc(0);
  const size_t n = x.size();
  double wsum = 0;
  for (size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2 * M_PI * static_cast<double>(i) / static_cast<double>(n));
    acc += x[i] * w * std::polar(1.0, -2 * M_PI * hz * static_cast<double>(i) / 48000.0);
    wsum += w;
  }
  return 2 * std::abs(acc) / wsum;
}

}  // namespace audio_test
