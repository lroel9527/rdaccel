#pragma once

// What the lossless part of the frames may cost: a bucket of bytes that
// fills with time at a share of the bitrate, holds at most a burst's worth,
// and is charged what each frame's lossless part really came to (so frames
// it had to let through, such as keyframes, hold back the next ones).
// Content that turns up with many colours at once (a video that paused for
// a moment, a window of photos) would otherwise go out in one burst: 20 kB
// is 0.4 s of a 0.4 Mbps link, and every frame after it waits.

#include <algorithm>
#include <cstdint>

namespace rdacore::session {

class LosslessBucket {
 public:
  // Time has passed until `now_us`: fill at `share` of `bps`, up to what
  // `burst_us` of `bps` holds. The first call fills it.
  void Fill(uint64_t now_us, int64_t bps, double share, double burst_us) {
    cap_ = static_cast<double>(bps) / 8.0 * burst_us / 1e6;
    if (!set_) {
      tokens_ = cap_;
      set_ = true;
    } else {
      const double gap_us = static_cast<double>(now_us - std::min(now_us, last_us_));
      tokens_ = std::min(cap_, tokens_ + static_cast<double>(bps) / 8.0 * share * gap_us / 1e6);
    }
    last_us_ = now_us;
  }
  // What a frame's lossless part came to (it may overdraw the bucket).
  void Charge(double bytes) { tokens_ -= bytes; }
  // The overdraft is not held against the frames after this one.
  void Forgive() { tokens_ = std::max(tokens_, 0.0); }

  double tokens() const { return tokens_; }  // bytes, may be negative
  double cap() const { return cap_; }        // what it holds at most, bytes

 private:
  double tokens_ = 0;
  double cap_ = 0;
  bool set_ = false;
  uint64_t last_us_ = 0;  // when it was last filled
};

}  // namespace rdacore::session
