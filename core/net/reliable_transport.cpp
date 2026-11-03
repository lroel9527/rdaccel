#include "net/reliable_transport.h"

#include <algorithm>
#include <chrono>

#include "capture/capturer.h"
#include "proto/wire.h"

namespace rdacore::net {
namespace {

enum PacketType : uint8_t { kData = 1, kAck = 2, kHello = 3 };

// Room for the data header: type, packet number, channel, offset.
constexpr size_t kMaxPayload = kMaxDatagram - 24;
constexpr uint64_t kInitialCwnd = 32 * kMaxDatagram;
constexpr uint64_t kMinCwnd = 4 * kMaxDatagram;
constexpr uint64_t kInitialRtt = 100'000;  // us, until measured
constexpr uint64_t kMaxAckDelay = 5'000;
constexpr uint64_t kPacingCredit = 15'000;  // us
constexpr uint64_t kMinRttWindow = 10'000'000;
constexpr uint64_t kBwWindowRounds = 10;
// ... and at most this long: once the bottleneck queue fills, a round lasts
// as long as the queue delay, and ten of them would keep a stale, higher
// rate for seconds after the path slows down (4 -> 1 Mbps took ~2.7 s).
constexpr uint64_t kBwWindowUs = 1'000'000;
constexpr double kStartupGain = 2.885;
constexpr double kProbeGains[8] = {1.25, 0.75, 1, 1, 1, 1, 1, 1};
constexpr double kQueueDrainGain = 0.9;
constexpr size_t kMaxAckRanges = 32;
// Sending order: small interactive channels first, then surface commands,
// application messages (possibly large: a pasted image), video last.
constexpr int kPriority[kChannelCount] = {0, 1, 4, 2, 5, 3};

uint64_t Now() { return capture::MonotonicMicros(); }

// Adds [s, e) to a set of disjoint ranges, merging neighbours.
void AddRange(std::map<uint64_t, uint64_t>& set, uint64_t s, uint64_t e) {
  if (s >= e) return;
  auto it = set.upper_bound(s);
  if (it != set.begin()) {
    auto prev = std::prev(it);
    if (prev->second >= s) {
      s = prev->first;
      e = std::max(e, prev->second);
      it = set.erase(prev);
    }
  }
  while (it != set.end() && it->first <= e) {
    e = std::max(e, it->second);
    it = set.erase(it);
  }
  set[s] = e;
}

}  // namespace

ReliableTransport::ReliableTransport(std::unique_ptr<DatagramSocket> socket, ReliableOptions options)
    : socket_(std::move(socket)), opt_(options) {
  socket_->SetReceiver([this](std::span<const uint8_t> d) { OnDatagram(d); });
  worker_ = std::thread([this] { Run(); });
}

ReliableTransport::~ReliableTransport() {
  // Linger briefly so the last control bytes (a Goodbye) reach the peer:
  // wait until they are acknowledged, at most kLingerUs.
  {
    constexpr uint64_t kLingerUs = 300'000;
    const uint64_t deadline = Now() + kLingerUs;
    const auto& control = out_[static_cast<size_t>(transport::Channel::kControl)];
    std::unique_lock<std::mutex> lock(mu_);
    while (!closed_ && heard_peer_ && !control.buf.empty() && Now() < deadline) {
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      lock.lock();
    }
  }
  Close();
  if (worker_.joinable()) worker_.join();
}

void ReliableTransport::Close() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
  }
  cv_.notify_all();
  recv_cv_.notify_all();
  socket_->Close();  // waits for a running receive callback
}

bool ReliableTransport::Send(transport::Channel ch, std::span<const uint8_t> bytes) {
  const auto c = static_cast<size_t>(ch);
  if (c >= kChannels) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) return false;
    out_[c].buf.insert(out_[c].buf.end(), bytes.begin(), bytes.end());
  }
  cv_.notify_all();
  return true;
}

void ReliableTransport::SetReceiver(ReceiveFn fn) {
  {
    std::lock_guard<std::mutex> lock(recv_mu_);
    receiver_ = std::move(fn);
  }
  recv_cv_.notify_all();
}

bool ReliableTransport::GetStats(TransportStats* out) const {
  std::lock_guard<std::mutex> lock(mu_);
  out->bandwidth_bps = static_cast<int64_t>(bw_ * 8e6);
  out->rtt_us = static_cast<uint32_t>(srtt_);
  out->queued_bytes = QueuedBytes();
  out->bytes_in_flight = bytes_in_flight_;
  out->packets_sent = packets_sent_;
  out->packets_lost = packets_lost_;
  return true;
}

bool ReliableTransport::PeerKnown() const {
  std::lock_guard<std::mutex> lock(mu_);
  return opt_.client || heard_peer_;
}

uint64_t ReliableTransport::QueuedBytes() const {
  uint64_t n = 0;
  for (const auto& s : out_) {
    n += s.base + s.buf.size() - s.next;
    for (const auto& [a, b] : s.resend) n += b - a;
  }
  return n;
}

uint64_t ReliableTransport::Cwnd() const {
  if (!opt_.congestion_control) return UINT64_MAX;
  if (bw_ <= 0 || min_rtt_ == 0) return kInitialCwnd;
  const double gain = mode_ == Mode::kStartup ? kStartupGain : 2.0;
  // A few packets on top absorb delayed and stretched acknowledgements.
  const auto bdp = static_cast<uint64_t>(gain * bw_ * static_cast<double>(min_rtt_));
  const uint64_t cwnd = std::max(bdp + 4 * kMaxDatagram, kMinCwnd);
  // Like the pacing rate, the window does not shrink below its initial
  // size before the path has been measured under load.
  return mode_ == Mode::kStartup ? std::max(cwnd, kInitialCwnd) : cwnd;
}

bool ReliableTransport::PathQueued() const {
  // The smoothed round trip: single samples jitter by several ms where
  // threads wake late (acknowledgements go out late too), which would
  // shrink the window on a path with no queue at all.
  return min_rtt_ > 0 && srtt_ > min_rtt_ + std::max<uint64_t>(min_rtt_ / 4, 5'000) + kMaxAckDelay;
}

double ReliableTransport::PacingRate() const {
  if (!pacing_rate_) {
    return kStartupGain * static_cast<double>(kInitialCwnd) / static_cast<double>(kInitialRtt);
  }
  return pacing_rate_;
}

void ReliableTransport::UpdatePacingRate() {
  double gain = 1;
  switch (mode_) {
    case Mode::kStartup: gain = kStartupGain; break;
    case Mode::kDrain: gain = 1 / kStartupGain; break;
    case Mode::kProbeBw:
      gain = kProbeGains[cycle_];
      // Interactive traffic (cursor, input) shares the bottleneck queue
      // with video, so a standing queue there is latency for everything.
      // The max filter tends to sit a little above the true rate (acks
      // arrive in bunches), which would keep the queue as long as the
      // window allows. While the round trip shows a queue, send a little
      // slower than the estimate until it drains. The probe up (one round
      // in eight) still runs: samples can only show what we send, so a
      // round trip inflated by late threads rather than a queue (macOS CI)
      // would otherwise hold the rate below the estimate for good, and every
      // late acknowledgement would ratchet the estimate further down.
      if (cycle_ >= 2 && PathQueued()) gain = kQueueDrainGain;
      break;
  }
  const double rate = gain * bw_;
  // During startup the rate only goes up: an early sample taken while the
  // application had little to send must not throttle the first big frame.
  if (mode_ != Mode::kStartup || rate > PacingRate()) pacing_rate_ = rate;
}

uint64_t ReliableTransport::Pto() const {
  if (srtt_ == 0) return 3 * kInitialRtt;
  return srtt_ + std::max<uint64_t>(4 * rttvar_, 1000) + kMaxAckDelay;
}

void ReliableTransport::Run() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!closed_) {
    const uint64_t now = Now();
    if (opt_.client && !heard_peer_ && now >= next_hello_) {
      const uint8_t hello = kHello;
      socket_->Send({&hello, 1});
      next_hello_ = now + 100'000;
    }
    if (ack_pending_ > 0 && now >= ack_deadline_) SendAck(now);

    // Probe timeout: nothing acknowledged for too long. Resend the oldest
    // packet as a probe; its acknowledgement declares the rest lost (by the
    // rules in HandleAck) if they were. Declaring everything lost here made
    // a growing queue (the path slowing down) look like total loss: the
    // late acknowledgements of the originals were then ignored, so neither
    // the round trip nor the estimate moved, and the duplicates took most of
    // the slower path (12 kB/s of 50 after 4 -> 0.4 Mbps).
    if (!in_flight_.empty()) {
      const uint64_t since = std::max(last_ack_time_, in_flight_.begin()->second.time);
      if (now >= since + (Pto() << std::min(pto_count_, 6))) {
        auto oldest = in_flight_.begin();
        MarkLost(oldest->first, oldest->second);
        in_flight_.erase(oldest);
        ++pto_count_;
        last_ack_time_ = now;
      }
    }

    // Send while the window and the pacer allow. A listening side has no
    // peer address before the peer's first packet: hold the data until then.
    const bool can_send = opt_.client || heard_peer_;
    for (; can_send;) {
      const uint64_t t = Now();
      if (bytes_in_flight_ + kMaxDatagram > Cwnd()) break;
      if (opt_.congestion_control && t < next_send_time_) break;
      if (!SendOne(t)) break;
    }

    // Sleep until the next thing to do.
    uint64_t wake = now + 50'000;
    if (ack_pending_ > 0) wake = std::min(wake, ack_deadline_);
    if (!in_flight_.empty()) {
      const uint64_t since = std::max(last_ack_time_, in_flight_.begin()->second.time);
      wake = std::min(wake, since + (Pto() << std::min(pto_count_, 6)));
    }
    if (opt_.client && !heard_peer_) wake = std::min(wake, next_hello_);
    const bool have_data = can_send && QueuedBytes() > 0;
    if (have_data && opt_.congestion_control && bytes_in_flight_ + kMaxDatagram <= Cwnd()) {
      wake = std::min(wake, next_send_time_);
    }
    const uint64_t t = Now();
    if (wake > t) {
      cv_.wait_for(lock, std::chrono::microseconds(wake - t));
    }
  }
}

bool ReliableTransport::SendOne(uint64_t now) {
  if (QueuedBytes() == 0) {
    backlog_since_ = UINT64_MAX;
    return false;
  }
  if (backlog_since_ == UINT64_MAX) backlog_since_ = now;
  int ch = -1;
  uint64_t off = 0, len = 0;
  for (int c : kPriority) {
    SendStream& s = out_[c];
    // Lost ranges first, skipping whatever was acknowledged meanwhile.
    while (!s.resend.empty()) {
      auto it = s.resend.begin();
      uint64_t a = std::max(it->first, s.base), b = it->second;
      auto ack = s.acked.upper_bound(a);
      if (ack != s.acked.begin() && std::prev(ack)->second > a) a = std::prev(ack)->second;
      s.resend.erase(it);
      if (a >= b) continue;
      if (ack != s.acked.end() && ack->first < b) {
        // Stop at the next acknowledged range; the rest stays queued.
        AddRange(s.resend, ack->first, b);
        b = ack->first;
      }
      len = std::min<uint64_t>(b - a, kMaxPayload);
      if (a + len < b) AddRange(s.resend, a + len, b);
      ch = c;
      off = a;
      break;
    }
    if (ch >= 0) break;
    if (s.next < s.base + s.buf.size()) {
      ch = c;
      off = s.next;
      len = std::min<uint64_t>(s.base + s.buf.size() - s.next, kMaxPayload);
      s.next += len;
      break;
    }
  }
  if (ch < 0) return false;

  const SendStream& s = out_[ch];
  proto::ByteWriter w;
  w.u8(kData);
  const uint64_t pn = next_pn_++;
  w.varint(pn);
  w.u8(static_cast<uint8_t>(ch));
  w.varint(off);
  std::vector<uint8_t> d = w.take();
  const auto first = s.buf.begin() + static_cast<std::ptrdiff_t>(off - s.base);
  d.insert(d.end(), first, first + static_cast<std::ptrdiff_t>(len));
  socket_->Send(d);
  ++packets_sent_;

  if (in_flight_.empty()) {
    first_sent_time_ = now;
    delivered_time_ = now;
  }
  SentPacket p;
  p.ch = static_cast<uint8_t>(ch);
  p.offset = off;
  p.len = len;
  p.size = static_cast<uint32_t>(d.size());
  p.time = now;
  p.delivered = delivered_;
  p.delivered_time = delivered_time_;
  p.first_sent_time = first_sent_time_;
  // Application limited: what is queued after this packet would not fill
  // the window, so its delivery rate measures our sending, not the path.
  // Such samples may raise the estimate but never lower it.
  // Bursts (one video frame after an idle gap) count as application limited
  // until the sender has been backlogged for a round trip, as their first
  // samples span the idle gap.
  p.app_limited = !opt_.congestion_control || bytes_in_flight_ + p.size + QueuedBytes() < Cwnd() ||
                  now - backlog_since_ < std::max<uint64_t>(min_rtt_, 1000);
  // Window limited: no room for another packet. The window is two minimum
  // round trips of the estimate, so where the round trip grows for other
  // reasons than a queue (late threads on a loaded machine), its delivery
  // rate measures the window, not the path.
  p.cwnd_limited = opt_.congestion_control && bytes_in_flight_ + p.size + kMaxDatagram > Cwnd();
  in_flight_[pn] = p;
  bytes_in_flight_ += p.size;
  if (opt_.congestion_control) {
    // Credit for time the sender thread overslept: timers on a loaded
    // machine (macOS CI runners especially) wake several ms late, and
    // without credit each wake would send a single packet and the measured
    // bandwidth would spiral down. Bounded, so an idle sender cannot burst
    // more than kPacingCredit worth of packets.
    const uint64_t base = std::max(next_send_time_, now > kPacingCredit ? now - kPacingCredit : 0);
    next_send_time_ = base + static_cast<uint64_t>(static_cast<double>(p.size) / PacingRate());
  }
  return true;
}

void ReliableTransport::MarkLost(uint64_t, const SentPacket& p) {
  bytes_in_flight_ -= p.size;
  ++packets_lost_;
  AddRange(out_[p.ch].resend, p.offset, p.offset + p.len);
}

void ReliableTransport::ReleaseAcked(SendStream& s) {
  while (!s.acked.empty() && s.acked.begin()->first <= s.base) {
    const uint64_t end = s.acked.begin()->second;
    s.acked.erase(s.acked.begin());
    if (end > s.base) {
      const uint64_t n = std::min<uint64_t>(end - s.base, s.buf.size());
      s.buf.erase(s.buf.begin(), s.buf.begin() + static_cast<std::ptrdiff_t>(n));
      s.base += n;
    }
  }
  while (!s.resend.empty() && s.resend.begin()->second <= s.base) s.resend.erase(s.resend.begin());
}

void ReliableTransport::OnRateSample(const SentPacket& p, uint64_t now) {
  // A new round starts when a packet sent after the previous round's start
  // is acknowledged.
  bool round_start = false;
  if (p.delivered >= next_round_delivered_) {
    ++round_;
    next_round_delivered_ = delivered_;
    round_start = true;
  }
  const uint64_t send_elapsed = p.time - p.first_sent_time;
  const uint64_t ack_elapsed = now - p.delivered_time;
  const uint64_t interval = std::max(send_elapsed, ack_elapsed);
  // The next sample's send interval starts at this packet.
  first_sent_time_ = p.time;
  if (interval > 0 && (min_rtt_ == 0 || interval >= min_rtt_ / 2)) {
    const double rate = static_cast<double>(delivered_ - p.delivered) / static_cast<double>(interval);
    // A sample may lower the estimate only when the path set its pace (the
    // acknowledgements came back slower than the packets went out). When
    // they came back as fast, the sample measured our own sending: below
    // the estimate while draining a queue, or while threads woke late
    // (macOS CI), and taking it would spiral the estimate down.
    // Nor when the window, not the path, limited it: below the estimate it
    // would shrink the window and so the next sample (a spiral to almost
    // nothing on macOS CI, where the round trip ran at three times its
    // minimum with no queue).
    // While a queue stands at the bottleneck (the round trip far above its
    // minimum), though, the path sets the pace whatever we sent, so then an
    // application-limited sample counts too: a sender below the old rate
    // would otherwise keep a stale estimate for seconds after a drop.
    const bool path_paced = ack_elapsed > send_elapsed + send_elapsed / 8;
    const bool queued = min_rtt_ > 0 && latest_rtt_ > 2 * min_rtt_ + 20'000;
    if (((!p.app_limited || queued) && !p.cwnd_limited && path_paced) || rate >= bw_) {
      bw_samples_.push_back({round_, now, rate});
    }
  }
  // A standing queue at the bottleneck (the round trip well above its
  // minimum while we are backlogged) means we send faster than the path
  // delivers: the path has slowed down. Samples from before the last round
  // trip then describe the old path, so only the recent ones count.
  // Only those from before the queue began, though: the ones taken while
  // it stands describe the path as it is now and keep the full window.
  // (Cutting the window to one round trip for as long as the round trip
  // stayed high spiralled the estimate down where threads wake late: a
  // round trip three times its minimum with no queue on macOS CI, a window
  // too short for the probe up to count, and the pacing 10% below it.)
  uint64_t oldest = now > kBwWindowUs ? now - kBwWindowUs : 0;
  if (mode_ == Mode::kProbeBw && min_rtt_ > 0 && latest_rtt_ > 2 * min_rtt_ + 20'000) {
    if (queue_since_ == 0) queue_since_ = now;
    oldest = std::max(oldest, queue_since_ > srtt_ ? queue_since_ - srtt_ : 0);
  } else {
    queue_since_ = 0;
  }
  while (bw_samples_.size() > 1 && (bw_samples_.front().round + kBwWindowRounds < round_ ||
                                    bw_samples_.front().time < oldest)) {
    bw_samples_.pop_front();
  }
  double best = 0;
  for (const auto& s : bw_samples_) best = std::max(best, s.bw);
  if (best > 0) bw_ = best;

  switch (mode_) {
    case Mode::kStartup:
      if (round_start && !p.app_limited) {
        if (bw_ >= full_bw_ * 1.25) {
          full_bw_ = bw_;
          full_bw_rounds_ = 0;
        } else if (++full_bw_rounds_ >= 3) {
          mode_ = Mode::kDrain;
        }
      }
      break;
    case Mode::kDrain:
      if (static_cast<double>(bytes_in_flight_) <= bw_ * static_cast<double>(min_rtt_) ||
          bytes_in_flight_ <= kMinCwnd) {
        mode_ = Mode::kProbeBw;
        cycle_ = 2;
        cycle_start_ = now;
      }
      break;
    case Mode::kProbeBw:
      if (now - cycle_start_ > std::max<uint64_t>(min_rtt_, 1000)) {
        cycle_ = (cycle_ + 1) % 8;
        cycle_start_ = now;
      }
      break;
  }
  UpdatePacingRate();
}

void ReliableTransport::HandleAck(std::span<const uint8_t> payload, uint64_t now) {
  proto::ByteReader r(payload);
  uint64_t largest = 0, ack_delay = 0, n = 0, count = 0;
  if (!(r.varint(largest) && r.varint(ack_delay) && r.varint(n) && r.varint(count)) ||
      n == 0 || n > kMaxAckRanges || count > largest) {
    return;
  }
  std::vector<std::pair<uint64_t, uint64_t>> ranges{{largest - count, largest}};
  for (uint64_t i = 1; i < n; ++i) {
    uint64_t gap = 0;
    if (!(r.varint(gap) && r.varint(count))) return;
    const uint64_t lo = ranges.back().first;
    if (lo < gap + 2) return;
    const uint64_t hi = lo - gap - 2;
    if (count > hi) return;
    ranges.push_back({hi - count, hi});
  }

  bool newest_acked = false;
  const SentPacket* newest = nullptr;
  SentPacket newest_copy;
  bool touched[kChannels] = {};
  for (const auto& [lo, hi] : ranges) {
    for (auto it = in_flight_.lower_bound(lo); it != in_flight_.end() && it->first <= hi;) {
      const SentPacket& p = it->second;
      if (it->first == largest) newest_acked = true;
      bytes_in_flight_ -= p.size;
      delivered_ += p.size;
      delivered_time_ = now;
      AddRange(out_[p.ch].acked, p.offset, p.offset + p.len);
      touched[p.ch] = true;
      if (!newest || p.time > newest->time) {
        newest_copy = p;
        newest = &newest_copy;
      }
      it = in_flight_.erase(it);
    }
  }
  if (!newest) return;  // nothing new (duplicate or already declared lost)
  for (int c = 0; c < kChannels; ++c) {
    if (touched[c]) ReleaseAcked(out_[c]);
  }

  if (newest_acked) {
    latest_rtt_ = now - newest->time;
    if (min_rtt_ == 0 || latest_rtt_ < min_rtt_ || now - min_rtt_time_ > kMinRttWindow) {
      min_rtt_ = latest_rtt_;
      min_rtt_time_ = now;
    }
    uint64_t adjusted = latest_rtt_;
    if (adjusted > min_rtt_ + ack_delay) adjusted -= ack_delay;
    if (srtt_ == 0) {
      srtt_ = adjusted;
      rttvar_ = adjusted / 2;
    } else {
      const uint64_t diff = srtt_ > adjusted ? srtt_ - adjusted : adjusted - srtt_;
      rttvar_ = (3 * rttvar_ + diff) / 4;
      srtt_ = (7 * srtt_ + adjusted) / 8;
    }
  }
  OnRateSample(*newest, now);

  if (!any_acked_ || largest > largest_acked_) largest_acked_ = largest;
  any_acked_ = true;
  last_ack_time_ = now;
  pto_count_ = 0;

  // Loss: three newer packets acknowledged, or sent well over an RTT before
  // something that was acknowledged.
  const uint64_t threshold = std::max<uint64_t>(9 * std::max(srtt_, latest_rtt_) / 8, 1000);
  for (auto it = in_flight_.begin(); it != in_flight_.end() && it->first < largest_acked_;) {
    if (largest_acked_ - it->first >= 3 || it->second.time + threshold < now) {
      MarkLost(it->first, it->second);
      it = in_flight_.erase(it);
    } else {
      ++it;
    }
  }
}

void ReliableTransport::SendAck(uint64_t now) {
  if (received_.empty()) return;
  proto::ByteWriter w;
  w.u8(kAck);
  auto it = received_.rbegin();
  w.varint(it->second);
  w.varint(now - largest_received_time_);
  const size_t n = std::min(received_.size(), kMaxAckRanges);
  w.varint(n);
  w.varint(it->second - it->first);
  uint64_t lo = it->first;
  for (size_t i = 1; i < n; ++i) {
    ++it;
    w.varint(lo - it->second - 2);
    w.varint(it->second - it->first);
    lo = it->first;
  }
  socket_->Send(w.data());
  ack_pending_ = 0;
}

void ReliableTransport::OnDatagram(std::span<const uint8_t> d) {
  if (d.empty()) return;
  const uint64_t now = Now();
  std::vector<std::pair<int, std::vector<uint8_t>>> deliver;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) return;
    if (!heard_peer_) {
      heard_peer_ = true;
      cv_.notify_all();  // data held for the peer can go now
    }
    proto::ByteReader r(d.subspan(1));
    if (d[0] == kAck) {
      HandleAck(d.subspan(1), now);
    } else if (d[0] == kData) {
      uint64_t pn = 0, off = 0;
      uint8_t ch = 0;
      std::span<const uint8_t> data;
      if (!(r.varint(pn) && r.u8(ch) && r.varint(off)) || ch >= kChannels ||
          !r.bytes(r.remaining(), data)) {
        return;
      }
      const bool in_order = received_.empty() || pn == received_.rbegin()->second + 1;
      const bool newest = received_.empty() || pn > received_.rbegin()->second;
      // Ranges are stored as [lo, hi] (inclusive).
      {
        uint64_t lo = pn, hi = pn;
        auto it = received_.upper_bound(pn);
        if (it != received_.begin()) {
          auto prev = std::prev(it);
          if (prev->second + 1 >= pn) {
            lo = prev->first;
            hi = std::max(hi, prev->second);
            received_.erase(prev);
          }
        }
        it = received_.upper_bound(hi);
        if (it != received_.end() && it->first == hi + 1) {
          hi = it->second;
          received_.erase(it);
        }
        received_[lo] = hi;
        while (received_.size() > kMaxAckRanges) received_.erase(received_.begin());
      }
      if (newest) largest_received_time_ = now;
      // Acknowledge every second packet, at once on reordering or loss,
      // otherwise within kMaxAckDelay.
      if (ack_pending_++ == 0) ack_deadline_ = now + kMaxAckDelay;
      if (ack_pending_ >= 2 || !in_order) ack_deadline_ = now;
      cv_.notify_all();

      RecvStream& s = in_[ch];
      if (off + data.size() > s.next) {
        if (off <= s.next) {
          deliver.emplace_back(ch, std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(s.next - off),
                                                        data.end()));
          s.next = off + data.size();
          // Anything buffered that now continues the stream.
          for (auto it = s.pending.begin(); it != s.pending.end() && it->first <= s.next;) {
            const uint64_t end = it->first + it->second.size();
            if (end > s.next) {
              deliver.back().second.insert(deliver.back().second.end(),
                                           it->second.begin() + static_cast<std::ptrdiff_t>(s.next - it->first),
                                           it->second.end());
              s.next = end;
            }
            it = s.pending.erase(it);
          }
        } else {
          auto& slot = s.pending[off];
          if (slot.size() < data.size()) slot.assign(data.begin(), data.end());
        }
      }
    }
  }
  if (deliver.empty()) return;
  ReceiveFn fn;
  {
    // Hold data until a receiver is installed, as the other transports do.
    std::unique_lock<std::mutex> lock(recv_mu_);
    recv_cv_.wait(lock, [&] {
      std::lock_guard<std::mutex> l(mu_);
      return receiver_ || closed_;
    });
    fn = receiver_;
  }
  if (!fn) return;
  for (auto& [ch, bytes] : deliver) fn(static_cast<transport::Channel>(ch), bytes);
}

}  // namespace rdacore::net
