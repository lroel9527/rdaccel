#include "audio/resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rdacore::audio {
namespace {

constexpr int kHalf = Resampler::kTaps / 2;
constexpr double kCutoff = 22'000.0 / 48'000.0;  // cycles per input sample
constexpr double kBeta = 8.6;                    // Kaiser: about -90 dB side lobes

double BesselI0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 50; ++k) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return sum;
}

}  // namespace

Resampler::Resampler(int channels, size_t capacity)
    : channels_(channels), capacity_(capacity + 2 * kTaps) {
  // table_[p][j]: the weight of input frame (centre + j - kHalf + 1) for an
  // output frac = p / kPhases past the centre.
  table_.resize(static_cast<size_t>(kPhases + 1) * kTaps);
  const double i0b = BesselI0(kBeta);
  for (int p = 0; p <= kPhases; ++p) {
    const double frac = static_cast<double>(p) / kPhases;
    double sum = 0;
    float* row = &table_[static_cast<size_t>(p) * kTaps];
    for (int j = 0; j < kTaps; ++j) {
      const double t = (j - kHalf + 1) - frac;  // input samples from the output point
      const double x = 2 * kCutoff * t;
      const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
      const double w = t / kHalf;
      const double win = std::abs(w) >= 1 ? 0.0 : BesselI0(kBeta * std::sqrt(1 - w * w)) / i0b;
      const double h = 2 * kCutoff * sinc * win;
      row[j] = static_cast<float>(h);
      sum += h;
    }
    for (int j = 0; j < kTaps; ++j) row[j] = static_cast<float>(row[j] / sum);  // unity gain at DC
  }
  buf_.assign(capacity_ * static_cast<size_t>(channels_), 0.0f);
  Reset();
}

void Resampler::Reset() {
  std::fill(buf_.begin(), buf_.end(), 0.0f);
  // Zero history before the first input: the first output is centred on
  // the last zero.
  pos_ = kHalf - 1;
  end_ = kHalf;
  frac_ = 0;
}

void Resampler::Compact() {
  const size_t keep_from = pos_ - (kHalf - 1);
  if (keep_from == 0) return;
  std::memmove(buf_.data(), buf_.data() + keep_from * channels_, (end_ - keep_from) * channels_ * sizeof(float));
  pos_ -= keep_from;
  end_ -= keep_from;
}

size_t Resampler::Write(const float* in, size_t frames) {
  if (end_ + frames > capacity_) Compact();
  const size_t n = std::min(frames, capacity_ - end_);
  std::memcpy(buf_.data() + end_ * channels_, in, n * channels_ * sizeof(float));
  end_ += n;
  return n;
}

size_t Resampler::Needed(size_t frames, double ratio) const {
  // The last output frame is centred at pos_ + frac_ + (frames - 1) * ratio
  // and reads kHalf frames past its centre.
  if (frames == 0) return 0;
  const double last = frac_ + static_cast<double>(frames - 1) * ratio;
  return static_cast<size_t>(std::floor(last)) + kHalf + 1;
}

size_t Resampler::Read(float* out, size_t frames, double ratio) {
  size_t done = 0;
  for (; done < frames; ++done) {
    if (pos_ + kHalf >= end_) break;  // needs frames up to pos_ + kHalf
    const double p = frac_ * kPhases;
    const int p0 = std::min(static_cast<int>(p), kPhases - 1);
    const float w = static_cast<float>(p - p0);
    const float* a = &table_[static_cast<size_t>(p0) * kTaps];
    const float* b = a + kTaps;
    const float* x = buf_.data() + (pos_ - (kHalf - 1)) * channels_;
    for (int c = 0; c < channels_; ++c) {
      float acc = 0;
      for (int j = 0; j < kTaps; ++j) acc += (a[j] + w * (b[j] - a[j])) * x[j * channels_ + c];
      out[done * channels_ + c] = acc;
    }
    frac_ += ratio;
    const double whole = std::floor(frac_);
    pos_ += static_cast<size_t>(whole);
    frac_ -= whole;
  }
  return done;
}

}  // namespace rdacore::audio
