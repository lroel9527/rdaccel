#include "session/rate_control.h"

#include <algorithm>

namespace rdacore::session {

RateController::Decision RateController::Update(uint64_t now_us, const TransportStats& st,
                                                uint64_t sent_bytes, uint64_t total_bytes) {
  Decision d;
  const int64_t drain_bps = st.bandwidth_bps > 0 ? st.bandwidth_bps : target_;
  queue_delay_us_ = drain_bps > 0 ? st.queued_bytes * 8'000'000 / static_cast<uint64_t>(drain_bps) : 0;
  if (st.rtt_us > 0 && (min_rtt_us_ == 0 || st.rtt_us < min_rtt_us_)) min_rtt_us_ = st.rtt_us;
  const bool path_queue = min_rtt_us_ > 0 && st.rtt_us > 2 * min_rtt_us_ + max_queue_us_;
  // A queue inside the network counts too once the target is at its floor:
  // an encoder that sends more than the path takes even there (VideoToolbox
  // at 300 kbps sends ~500) would otherwise keep that queue standing, with
  // the transport's own queue empty (the window lets it all out). Every
  // other frame only: the round trip is measured on what is sent, so with
  // nothing sent it would stay high and keep every frame back.
  const bool halve = path_queue && target_ <= min_bps_ && (++path_skips_ & 1) != 0;
  // Once the encoder is asked for no more than the floor (the target is
  // there, or what it produced beyond what it was asked brought its rate
  // there), keep what goes out within 90% of the estimate whatever it does:
  // a frame is skipped while the bytes sent overdraw that budget.
  // (VideoToolbox spends a near-fixed amount on each picture of a busy
  // 1080p screen, ~490 kbps at 30 fps however low it is asked to go; only
  // fewer pictures bring that down.) Up to 300 ms of the budget is saved.
  // A skipped frame keeps the target at the floor (see below).
  // Above the floor the budget stays off: the estimate is only a lower
  // bound there, and holding the sender to it would keep it one.
  bool over_budget = false;
  if (total_bytes > 0 && st.bandwidth_bps > 0 &&
      static_cast<double>(target_) * scale_ <= static_cast<double>(min_bps_)) {
    const double rate = 0.9 * static_cast<double>(st.bandwidth_bps) / 8e6;  // bytes per us
    const double cap = rate * 300'000;
    if (budget_us_ == 0) {
      budget_ = 0;  // nothing saved yet: the floor is reached after a queue
    } else {
      budget_ = std::min(cap, budget_ + rate * static_cast<double>(now_us - budget_us_));
      budget_ -= static_cast<double>(total_bytes - std::min(total_bytes, budget_bytes_));
    }
    budget_us_ = now_us;
    budget_bytes_ = total_bytes;
    over_budget = budget_ < 0;
  } else {
    budget_us_ = 0;
  }
  d.skip_frame = queue_delay_us_ > max_queue_us_ || halve || over_budget;
  if (d.skip_frame) ++skipped_;

  // The path fell: the estimate (measured from what was delivered) is under
  // half of what it was. The target follows at once, not 10% a step: at
  // 4 -> 0.4 Mbps it stayed at 4.6 Mbps for 0.4 s after the estimate had
  // fallen, and the frames of that time (a 15 kB picture, an 11 kB lossless
  // part) took more than a second to get through.
  bool fell = false;
  if (st.bandwidth_bps > 0) {
    peak_bps_ = std::max(peak_bps_, st.bandwidth_bps);
    fell = st.bandwidth_bps * 2 < peak_bps_ && target_ > st.bandwidth_bps;
  }
  constexpr uint64_t kInterval = 250'000;
  if (started_ && !fell && now_us - last_update_us_ < kInterval) return d;
  started_ = true;
  last_update_us_ = now_us;
  if (st.bandwidth_bps <= 0) return d;

  int64_t next;
  const bool skipped = skipped_ > 0;
  skipped_ = 0;
  const bool draining = st.queued_bytes < last_queued_;
  last_queued_ = st.queued_bytes;
  if (fell) {
    congested_us_ = now_us;
    congested_bps_ = st.bandwidth_bps;
    peak_bps_ = st.bandwidth_bps;
    next = std::min(target_, st.bandwidth_bps * 85 / 100);
  } else if (skipped || path_queue || queue_delay_us_ > max_queue_us_ / 2) {
    // The queue is building (frames had to be skipped), so the sender is
    // not application limited and the estimate is the path's rate: back off
    // below it at once. Fewer bits per frame beat fewer frames. Below that,
    // keep backing off slowly only while the queue is not draining (one big
    // picture, such as the first one, is not a reason to starve the rest).
    congested_us_ = now_us;
    congested_bps_ = st.bandwidth_bps;
    const int64_t cap = st.bandwidth_bps * 85 / 100;
    next = target_ > cap ? cap : (draining ? target_ : target_ * 95 / 100);
  } else {
    // While the queue stays short, the estimate only shows what we sent
    // (a lower bound of the path), so grow gradually instead of capping at
    // it; a building queue will stop the growth. Twice the estimate stays
    // the ceiling, though, also on the way down: a target left at 8 Mbps
    // after the path fell to 1 (the encoder sending less than either) would
    // let the next busy scene overshoot the path eightfold. Down by 10% a
    // step at most, as the first estimates of a session are still low.
    // After a queue built up, though, the estimate was the path's rate, and
    // twice it overfills the path as soon as the screen gets busy: the
    // network's buffer overflows, and every frame behind a lost packet waits
    // for its retransmission (a sawtooth of 1-2 s delays every few seconds
    // at 0.4 Mbps). So the ceiling starts at the estimate then and opens up
    // by 10% of it per second, back to twice it after ten seconds.
    int64_t ceiling = st.bandwidth_bps * 2;
    if (congested_us_ != 0) {
      const double opened = std::min(1.0, static_cast<double>(now_us - congested_us_) / 10e6);
      const auto base = static_cast<double>(std::max(st.bandwidth_bps, congested_bps_));
      ceiling = std::min(ceiling, static_cast<int64_t>(base * (1.0 + opened)));
    }
    next = std::min(target_ + target_ / 4, std::max(ceiling, min_bps_));
    next = std::max(next, target_ * 9 / 10);
  }
  target_ = std::clamp(next, min_bps_, max_bps_);

  // Overshoot: compare what was sent with what the encoder was asked for,
  // over at least a second at one setting (the encoder needs time to settle).
  if (sent_bytes > 0) {
    if (window_start_us_ == 0 || sent_bytes < window_bytes_) {
      window_start_us_ = now_us;
      window_bytes_ = sent_bytes;
    } else if (now_us - window_start_us_ >= 1'000'000) {
      const double out = static_cast<double>(sent_bytes - window_bytes_) * 8e6 /
                         static_cast<double>(now_us - window_start_us_);
      const auto asked = static_cast<double>(encoder_);
      const bool over = out > asked * 1.1;
      if (over && over_before_) {
        // Two seconds in a row, so not one big picture (a keyframe): a
        // step at a time, at most 30%.
        scale_ = std::max(0.25, scale_ * std::max(0.7, asked / out));
      } else if (!over && scale_ < 1) {
        // Keeping to it (or under it: a quiet screen, skipped frames): ease
        // back, 20% per second, in case the overshoot was passing.
        scale_ = std::min(1.0, scale_ * 1.2);
      }
      over_before_ = over;
      window_start_us_ = now_us;
      window_bytes_ = sent_bytes;
    }
  }
  const int64_t enc = std::max(min_bps_ / 2, static_cast<int64_t>(static_cast<double>(target_) * scale_));
  // Small changes are not worth an encoder reconfiguration.
  if (enc * 10 < encoder_ * 9 || enc * 10 > encoder_ * 11 || (enc == max_bps_ && encoder_ != max_bps_)) {
    encoder_ = enc;
    d.new_target_bps = enc;
    window_start_us_ = now_us;
    window_bytes_ = sent_bytes;
  }
  return d;
}

}  // namespace rdacore::session
