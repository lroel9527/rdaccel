#include "session/rate_control.h"

#include <algorithm>

namespace rdacore::session {

RateController::Decision RateController::Update(uint64_t now_us, const TransportStats& st) {
  Decision d;
  const int64_t drain_bps = st.bandwidth_bps > 0 ? st.bandwidth_bps : target_;
  queue_delay_us_ = drain_bps > 0 ? st.queued_bytes * 8'000'000 / static_cast<uint64_t>(drain_bps) : 0;
  d.skip_frame = queue_delay_us_ > max_queue_us_;
  if (d.skip_frame) ++skipped_;

  constexpr uint64_t kInterval = 250'000;
  if (started_ && now_us - last_update_us_ < kInterval) return d;
  started_ = true;
  last_update_us_ = now_us;
  if (st.bandwidth_bps <= 0) return d;

  int64_t next;
  const bool skipped = skipped_ > 0;
  skipped_ = 0;
  const bool draining = st.queued_bytes < last_queued_;
  last_queued_ = st.queued_bytes;
  if (st.rtt_us > 0 && (min_rtt_us_ == 0 || st.rtt_us < min_rtt_us_)) min_rtt_us_ = st.rtt_us;
  const bool path_queue = min_rtt_us_ > 0 && st.rtt_us > 2 * min_rtt_us_ + max_queue_us_;
  if (skipped || path_queue || queue_delay_us_ > max_queue_us_ / 2) {
    // The queue is building (frames had to be skipped), so the sender is
    // not application limited and the estimate is the path's rate: back off
    // below it at once. Fewer bits per frame beat fewer frames. Below that,
    // keep backing off slowly only while the queue is not draining (one big
    // picture, such as the first one, is not a reason to starve the rest).
    const int64_t cap = st.bandwidth_bps * 85 / 100;
    next = target_ > cap ? cap : (draining ? target_ : target_ * 95 / 100);
  } else {
    // While the queue stays short, the estimate only shows what we sent
    // (a lower bound of the path), so grow gradually instead of capping at
    // it; a building queue will stop the growth.
    next = std::min(target_ + target_ / 4, std::max(st.bandwidth_bps * 2, min_bps_));
    next = std::max(next, target_);
  }
  next = std::clamp(next, min_bps_, max_bps_);
  // Small changes are not worth an encoder reconfiguration.
  if (next * 10 < target_ * 9 || next * 10 > target_ * 11 || (next == max_bps_ && target_ != max_bps_)) {
    target_ = next;
    d.new_target_bps = next;
  }
  return d;
}

}  // namespace rdacore::session
