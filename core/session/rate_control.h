#pragma once

// Adaptive bitrate for the Host, driven by what the transport measures
// (rdacore::TransportStats):
//   * frames are skipped while the transport's send queue would take longer
//     than `max_queue_ms` to drain, so latency stays bounded on a slow link
//     (the next frame that is encoded carries every change since);
//   * when the queue builds or frames were skipped, the encoders' target bitrate drops at once to
//     85% of the estimated bandwidth; while it stays short the target grows
//     by 25% per step (at most to twice the estimate, which is only a lower
//     bound while the sender is application limited), between `min_bps` and
//     the configured bitrate;
//   * encoders do not always keep to the rate they are given (VideoToolbox
//     overshoots at low rates): when what they produced over a second
//     exceeds the target, the encoder is asked for proportionally less;
//   * at the floor, frames are also skipped while the session sends more
//     than 90% of the estimate (an encoder can only go so low per picture);
//   * a round trip well above the lowest one seen means a queue is building
//     inside the network (the path slowed down before the bandwidth estimate
//     noticed), which also counts as a building queue: max_queue_ms above
//     the lowest one. That has to hold for every reading of the last
//     kRecentWindowUs, so jitter (some
//     readings high, some low) is not a queue; and the lowest one is that of
//     the last kMinRttWindowUs, so a path whose base delay grew (another
//     Wi-Fi access point, a VPN) is not taken for a standing queue for good.

#include <cstdint>
#include <deque>
#include <utility>

#include "rdacore/transport.h"

namespace rdacore::session {

class RateController {
 public:
  RateController(int64_t max_bps, int64_t min_bps, int32_t max_queue_ms)
      : max_bps_(max_bps), min_bps_(min_bps), max_queue_us_(static_cast<uint64_t>(max_queue_ms) * 1000),
        target_(max_bps), encoder_(max_bps) {}

  struct Decision {
    bool skip_frame = false;
    int64_t new_target_bps = 0;  // 0 = unchanged
  };

  // `sent_bytes`: the encoders' output so far (HEVC payload; lossless tiles
  // are not theirs to correct), 0 = not known. It shows what they really
  // produce. `total_bytes`: everything the session sent so far, 0 = not
  // known; at the floor, frames are skipped to keep it within the estimate.
  Decision Update(uint64_t now_us, const TransportStats& st, uint64_t sent_bytes = 0,
                  uint64_t total_bytes = 0);

  static constexpr uint64_t kMinRttWindowUs = 10'000'000;
  // Until the estimate reaches min_bps or this long has passed, it is no
  // estimate yet (the first samples of a session, taken from a handshake and
  // a few bytes, read ~1 kbps): queue delay and back-off wait for a real one.
  static constexpr uint64_t kStartupUs = 2'000'000;
  static constexpr uint64_t kRecentWindowUs = 250'000;

  int64_t target_bps() const { return target_; }
  // What the encoder is asked for: the target, less what it was seen to
  // overshoot by.
  int64_t encoder_bps() const { return encoder_; }
  uint64_t queue_delay_us() const { return queue_delay_us_; }

 private:
  int64_t max_bps_, min_bps_;
  uint64_t max_queue_us_;
  int64_t target_;
  uint64_t last_update_us_ = 0;
  bool started_ = false;
  uint64_t queue_delay_us_ = 0;
  uint64_t skipped_ = 0;  // frames skipped since the last update
  uint64_t last_queued_ = 0;
  uint64_t path_skips_ = 0;  // frames considered for skipping on a path queue
  double budget_ = 0;        // bytes that may still go out at the floor
  uint64_t budget_us_ = 0, budget_bytes_ = 0;  // when it was last updated, and the bytes sent then
  // What the encoders produced at the last target update, to tell whether
  // they push against the target (see Update: the target grows only then).
  uint64_t growth_sent_ = 0, growth_us_ = 0;
  bool seen_update_ = false;
  uint64_t first_update_us_ = 0;
  bool estimate_credible_ = false;
  uint32_t min_rtt_us_ = 0;       // lowest smoothed RTT of the last kMinRttWindowUs
  uint64_t min_rtt_time_us_ = 0;  // when it was seen
  std::deque<std::pair<uint64_t, uint32_t>> recent_rtt_;  // {time, RTT} of the last kRecentWindowUs
  // The last time a queue built up, and the estimate then: the path's rate,
  // which the target then stays near for a while (see Update).
  uint64_t congested_us_ = 0;
  int64_t peak_bps_ = 0;  // highest estimate since the last fall (see Update)
  int64_t congested_bps_ = 0;
  int64_t encoder_ = 0;      // last rate given to the encoder (0 = the configured one)
  double scale_ = 1;         // encoder rate = target * scale
  bool over_before_ = false;  // the last window overshot too
  uint64_t window_start_us_ = 0, window_bytes_ = 0;  // output measured since the last change
};

}  // namespace rdacore::session
