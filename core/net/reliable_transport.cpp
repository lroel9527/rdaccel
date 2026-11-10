#include "net/reliable_transport.h"

#include <algorithm>
#include <chrono>

#include "capture/capturer.h"
#include "proto/wire.h"

namespace rdacore::net {
namespace {

enum PacketType : uint8_t { kData = 1, kAck = 2, kHello = 3, kFec = 4 };
// A parity packet in flight: not a channel's bytes, never resent.
constexpr uint8_t kFecChannel = 0xff;
constexpr size_t kMaxRecvBodies = 2048;
constexpr size_t kMaxPendingParities = 256;
constexpr uint64_t kFecMinRtt = 40'000;     // below it a resend is quick enough
constexpr uint64_t kFecHoldUs = 10'000'000;
constexpr double kFecShare = 0.3;

// Room for the data header: type, packet number, channel, offset.
constexpr size_t kMaxPayload = kMaxDatagram - 24;
constexpr uint64_t kInitialCwnd = 32 * kMaxDatagram;
constexpr uint64_t kMinCwnd = 2 * kMaxDatagram;
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
  Linger();
  Close();
  if (worker_.joinable()) worker_.join();
}

void ReliableTransport::Linger() {
  // Linger briefly so the last control bytes (a Goodbye) reach the peer:
  // wait until they are acknowledged, at most kLingerUs.
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

std::unique_ptr<transport::ITransport> ReliableTransport::NextPeer(std::string* error) {
  if (opt_.client) {
    if (error) *error = "a connecting transport has only its own peer";
    return nullptr;
  }
  Linger();
  Close();
  if (worker_.joinable()) worker_.join();
  auto next = socket_->NextPeer();
  if (!next) {
    if (error) *error = "this socket has a fixed peer";
    return nullptr;
  }
  return std::make_unique<ReliableTransport>(std::move(next), opt_);
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
  out->bandwidth_bps = static_cast<int64_t>(std::min(bw_ * 8e6, 1e15));
  out->rtt_us = static_cast<uint32_t>(srtt_);
  out->queued_bytes = QueuedBytes();
  out->bytes_in_flight = bytes_in_flight_;
  out->packets_sent = packets_sent_;
  out->packets_lost = packets_lost_;
  out->packets_spurious = packets_spurious_;
  out->fec_parity_sent = fec_parity_sent_;
  out->fec_recovered = peer_fec_recovered_;
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
  // Two round trips' worth: past startup, the minimum one plus the smoothed
  // one. Where the round trip runs well above its minimum without a queue
  // (threads that wake late hold packets and acknowledgements: 3x on macOS
  // CI), a window of two minimum round trips could not keep the path busy,
  // so its samples came in below the estimate, which shrank the window in
  // turn. The pacing rate, not the window, keeps the queue short.
  // While a queue stands, though, the smoothed round trip is held where it
  // was when the queue began: the queue raises the round trip, which would
  // widen the window, which lets the queue grow further (with acknowledge-
  // ments in bunches the estimate runs above the path, and the pacing rate
  // then cannot drain it: cursor latency doubled on a loaded macOS runner).
  // A few packets on top absorb delayed and stretched acknowledgements.
  const uint64_t srtt = queue_since_ != 0 ? std::min(srtt_, queue_srtt_) : srtt_;
  double rtt = static_cast<double>(min_rtt_);
  if (mode_ != Mode::kStartup) rtt = (rtt + static_cast<double>(std::max(srtt, min_rtt_))) / 2;
  // Capped: a nonsensical estimate (bogus acknowledgements) must not
  // overflow the conversion.
  const auto bdp = static_cast<uint64_t>(std::min(gain * bw_ * rtt, 1e15));
  // On a very slow path four packets are a long queue (0.38 s at 100 kbps,
  // more than many bottlenecks hold: the overflow showed as 8-17% loss on a
  // loss-free 100 kbps path), so there the allowance is 100 ms of the
  // estimate, two packets at least. From 0.4 Mbps up it is four as before.
  const auto extra = std::clamp(static_cast<uint64_t>(std::min(bw_ * 100'000.0, 1e15)),
                                static_cast<uint64_t>(2 * kMaxDatagram), static_cast<uint64_t>(4 * kMaxDatagram));
  const uint64_t cwnd = std::max(bdp + extra, kMinCwnd);
  // Like the pacing rate, the window does not shrink below its initial
  // size before the path has been measured under load.
  return mode_ == Mode::kStartup ? std::max(cwnd, kInitialCwnd) : cwnd;
}

uint64_t ReliableTransport::RecentMinRtt() const {
  uint64_t m = latest_rtt_;
  for (const auto& [t, r] : recent_rtt_) m = std::min(m, r);
  return m;
}

bool ReliableTransport::PathQueued() const {
  // The lowest recent round trip, not the smoothed one: on a jittery path
  // (Wi-Fi) the smoothed round trip sits well above the minimum with no
  // queue at all, and draining for it kept the sender far below the path
  // (an estimate of ~0.3 Mbps on 4 Mbps with 60 ms of jitter).
  return min_rtt_ > 0 && RecentMinRtt() > min_rtt_ + std::max<uint64_t>(min_rtt_ / 4, 5'000) + kMaxAckDelay;
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
    UpdateFec(now);

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
        MarkLost(oldest->first, oldest->second, now);
        in_flight_.erase(oldest);
        ++pto_count_;
        last_ack_time_ = now;
        probe_due_ = true;
      }
    }

    // Send while the window and the pacer allow. A listening side has no
    // peer address before the peer's first packet: hold the data until then.
    const bool can_send = opt_.client || heard_peer_;
    // The probe goes out even when the window is full. After the path slows
    // down sharply (4 -> 0.4 Mbps), the bottleneck drops the tail of what is
    // in flight, and nothing later is acknowledged to declare it lost; each
    // timeout then took out one packet, far too little to open the window,
    // so no probe was sent and the timeouts backed off to seconds (a 9 s
    // stall in the simulation).
    if (probe_due_ && can_send) {
      if (SendOne(Now())) probe_due_ = false;
    }
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
    if (backlog_since_ != UINT64_MAX) backlogged_us_ += now - backlog_since_;
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
  // Body ([channel][offset][bytes]) apart from the header: parities cover it.
  proto::ByteWriter bw;
  bw.u8(static_cast<uint8_t>(ch));
  bw.varint(off);
  std::vector<uint8_t> body = bw.take();
  const auto first = s.buf.begin() + static_cast<std::ptrdiff_t>(off - s.base);
  body.insert(body.end(), first, first + static_cast<std::ptrdiff_t>(len));
  proto::ByteWriter w;
  w.u8(kData);
  const uint64_t pn = next_pn_++;
  w.varint(pn);
  std::vector<uint8_t> d = w.take();
  d.insert(d.end(), body.begin(), body.end());
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
  if (fec_n_ >= 2) {
    fec::Add(fec_group_, pn, body);
    // A full group, or the end of a burst (a picture, a keystroke): what was
    // sent must be covered now, not when the next burst comes.
    const bool flush = now >= fec_no_flush_until_ && QueuedBytes() == 0;
    if (fec_group_.pns.size() >= fec_n_ || flush) SendParity(now);
  }
  return true;
}

void ReliableTransport::SendParity(uint64_t now) {
  if (fec_group_.pns.empty()) return;
  // Within the budget only; otherwise this group goes unprotected (a loss
  // in it is resent as without FEC).
  if (opt_.congestion_control && bw_ > 0) {
    // None while a queue stands at the bottleneck: the path is full, and
    // parities would only lengthen the queue.
    const double rate = PathQueued() || fec_busy_ ? 0.0 : kFecShare * bw_;  // bytes per us
    fec_tokens_ = std::min(fec_tokens_ + rate * static_cast<double>(now - std::min(now, fec_tokens_time_)),
                           std::max(rate * 100'000.0, 2.0 * kMaxDatagram));  // up to 100 ms of it
    fec_tokens_time_ = now;
    if (fec_tokens_ < static_cast<double>(fec_group_.body_xor.size())) {
      fec_group_ = {};
      return;
    }
    fec_tokens_ -= static_cast<double>(fec_group_.body_xor.size());
  }
  const uint64_t pn = next_pn_++;
  std::vector<uint8_t> d{kFec};
  const auto payload = fec::Encode(pn, fec_group_);
  d.insert(d.end(), payload.begin(), payload.end());
  socket_->Send(d);
  ++fec_parity_sent_;
  fec_group_ = {};
  // In flight like data, for the window, the pacing and the delivery rate;
  // never resent.
  SentPacket p;
  p.ch = kFecChannel;
  p.size = static_cast<uint32_t>(d.size());
  p.time = now;
  p.delivered = delivered_;
  p.delivered_time = delivered_time_;
  p.first_sent_time = first_sent_time_;
  p.app_limited = true;  // it rides on what the data asked for
  in_flight_[pn] = p;
  bytes_in_flight_ += p.size;
  if (opt_.congestion_control) {
    next_send_time_ += static_cast<uint64_t>(static_cast<double>(p.size) / PacingRate());
  }
}

void ReliableTransport::UpdateFec(uint64_t now) {
  if (opt_.fec >= 0) {
    fec_n_ = opt_.fec >= 2 ? std::min<size_t>(static_cast<size_t>(opt_.fec), fec::kMaxGroup) : 0;
    return;
  }
  if (now - fec_check_time_ < 500'000) return;
  // Loss as the peer sees it: what was resent for real, plus what it rebuilt
  // (which was not resent, so it would not show otherwise and FEC would
  // switch itself off).
  const uint64_t lost = packets_lost_ - std::min(packets_lost_, packets_spurious_) + peer_fec_recovered_;
  const uint64_t sent = packets_sent_ - fec_check_sent_;
  if (sent >= 50) {
    const double rate = static_cast<double>(lost - std::min(lost, fec_check_lost_)) / static_cast<double>(sent);
    fec_loss_ = 0.7 * fec_loss_ + 0.3 * rate;
    fec_check_sent_ = packets_sent_;
    fec_check_lost_ = lost;
  }
  // Busy: data waited to go out for over a quarter of the time since the
  // last check.
  uint64_t backlogged = backlogged_us_ + (backlog_since_ != UINT64_MAX ? now - backlog_since_ : 0);
  if (fec_check_time_ != 0 && now > fec_check_time_) {
    fec_busy_ = 4 * (backlogged - std::min(backlogged, fec_check_backlogged_)) > now - fec_check_time_;
  }
  fec_check_backlogged_ = backlogged;
  fec_check_time_ = now;
  size_t n = 0;
  if (srtt_ >= kFecMinRtt && fec_loss_ >= 0.002) {
    if (fec_n_ == 0) fec_base_loss_ = fec_loss_;
    // Only a clear overflow: 1-2% of loss (plus resends of resends, late
    // packets) must not trip it, or most of the gain is lost (p95 160 -> 400
    // ms at 2% over 200 ms); an overfilled 1 Mbps path showed ~19%.
    if (fec_loss_ > std::max(0.06, 3 * fec_base_loss_)) {
      // FEC itself overfills the path: full groups only (~6%) for a while.
      fec_no_flush_until_ = now + kFecHoldUs;
      fec_base_loss_ = fec_loss_;
    }
    const double loss = now < fec_no_flush_until_ ? 0.0 : fec_loss_;
    n = loss < 0.01 ? 16 : loss < 0.03 ? 8 : loss < 0.08 ? 4 : 3;
    fec_hold_until_ = now + kFecHoldUs;
  } else if (now < fec_hold_until_) {
    n = fec_n_;
  }
  if (n == 0 && fec_n_ != 0) SendParity(now);  // cover what was collected
  fec_n_ = n;
}

void ReliableTransport::MarkLost(uint64_t pn, const SentPacket& p, uint64_t now) {
  bytes_in_flight_ -= p.size;
  if (p.ch == kFecChannel) return;  // a parity: nothing to resend, not data lost
  ++packets_lost_;
  lost_recent_[pn] = {now, p.size};
  // Late acknowledgements come within a few round trips; keep it small.
  while (lost_recent_.size() > 4096 ||
         (!lost_recent_.empty() && now - lost_recent_.begin()->second.time > 2'000'000 + 4 * srtt_)) {
    lost_recent_.erase(lost_recent_.begin());
  }
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
    const bool queued = min_rtt_ > 0 && RecentMinRtt() > 2 * min_rtt_ + 20'000;
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
  // The queue starts with the first round trip well above the minimum and
  // ends only once the smoothed one is back near it: jitter that dips the
  // single samples below the line must not start a new queue (and cut the
  // window again) every few round trips.
  if (mode_ != Mode::kProbeBw || min_rtt_ == 0 ||
      RecentMinRtt() < min_rtt_ + std::max<uint64_t>(min_rtt_ / 2, 10'000)) {
    queue_since_ = 0;
  } else if (queue_since_ == 0 && RecentMinRtt() > 2 * min_rtt_ + 20'000) {
    queue_since_ = now;
    queue_srtt_ = srtt_;
  }
  // Samples from before the queue describe the old path. For its first two
  // round trips, only the last round trip counts (the first samples it
  // takes span the change and still show part of the old rate); after that
  // the window keeps everything since the queue began, so a round trip
  // that stays high with no queue at all (threads that wake late) cannot
  // keep cutting it.
  // While the sender is application limited and no queue stands, nothing it
  // sends can show the path's rate, so the samples it has do not age out:
  // the estimate holds what was measured under load instead of falling to
  // whatever the last small burst showed (632 -> 72 kbps a second after the
  // first picture on a jittery 4 Mbps path, the bitrate kept at its floor).
  // A queue still ages them out, so a path that slowed down is followed: one
  // that stands (queue_since_), or the smoothed round trip climbing, which
  // shows it sooner (the recent minimum lags by design, as jitter must not
  // count): below 400 kbps an application-limited 640 kbps sender saw its
  // estimate hold for 1.9 s otherwise.
  const bool hold = p.app_limited && queue_since_ == 0 && min_rtt_ > 0 && srtt_ < 2 * min_rtt_ + 20'000;
  while (!hold && bw_samples_.size() > 1 && (bw_samples_.front().round + kBwWindowRounds < round_ ||
                                             bw_samples_.front().time < oldest)) {
    bw_samples_.pop_front();
  }
  // The cut for a queue keeps the last few samples, though, however old:
  // where a thread wakes late (macOS CI) a round trip can pass with no
  // acknowledgement at all, and the one sample left, taken across the
  // stall, set the estimate to a fraction of the path (134 kbps on 1 Mbps).
  if (queue_since_ != 0) {
    const uint64_t recent = now > srtt_ ? now - srtt_ : 0;
    const uint64_t cut = now - queue_since_ < 2 * srtt_
                             ? std::max(recent, queue_since_ - std::min(queue_since_, srtt_))
                             : queue_since_;
    constexpr size_t kKeep = 4;
    while (bw_samples_.size() > kKeep && bw_samples_.front().time < cut) bw_samples_.pop_front();
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
  // Since 1.4: how many of our packets the peer rebuilt (older peers stop
  // before it).
  // The latest count, not the largest: it goes down when a rebuilt packet
  // turns out to have been only late.
  if (uint64_t rebuilt = 0; r.varint(rebuilt)) peer_fec_recovered_ = rebuilt;

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
      if (p.ch < kChannels) {
        AddRange(out_[p.ch].acked, p.offset, p.offset + p.len);
        touched[p.ch] = true;
      }
      if (!newest || p.time > newest->time) {
        newest_copy = p;
        newest = &newest_copy;
      }
      it = in_flight_.erase(it);
    }
  }
  // Acknowledged although declared lost: the resend was spurious (the
  // original was late, not lost: jitter, reordering). Counted only. Loosening
  // the loss rules on it (as RACK does) was tried: it cut spurious resends
  // eightfold on a jittery path but found real losses later, and the worst
  // latency over 200 ms round trips grew from ~450 to ~600 ms
  // (rdaccel_netsim). A resend costs bandwidth; waiting costs latency.
  for (const auto& [lo, hi] : ranges) {
    for (auto it = lost_recent_.lower_bound(lo); it != lost_recent_.end() && it->first <= hi;) {
      ++packets_spurious_;
      // It was delivered: count it, or the delivery rate (the bandwidth
      // estimate) comes out low by every late packet on a jittery path.
      delivered_ += it->second.size;
      delivered_time_ = now;
      it = lost_recent_.erase(it);
    }
  }
  if (!newest) return;  // nothing new (duplicate or already declared lost)
  for (int c = 0; c < kChannels; ++c) {
    if (touched[c]) ReleaseAcked(out_[c]);
  }

  if (newest_acked) {
    latest_rtt_ = now - newest->time;
    recent_rtt_.emplace_back(now, latest_rtt_);
    const uint64_t window = std::clamp<uint64_t>(2 * srtt_, 100'000, 1'000'000);
    while (recent_rtt_.size() > 1 && now - recent_rtt_.front().first > window) recent_rtt_.pop_front();
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
  probe_due_ = false;

  // Loss: three newer packets acknowledged, or sent well over an RTT before
  // something that was acknowledged.
  const uint64_t threshold = std::max<uint64_t>(9 * std::max(srtt_, latest_rtt_) / 8, 1000);
  for (auto it = in_flight_.begin(); it != in_flight_.end() && it->first < largest_acked_;) {
    if (largest_acked_ - it->first >= 3 || it->second.time + threshold < now) {
      MarkLost(it->first, it->second, now);
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
  w.varint(fec_recovered_);
  socket_->Send(w.data());
  ack_pending_ = 0;
}

void ReliableTransport::MarkReceived(uint64_t pn, uint64_t now, bool in_order_hint) {
  const bool in_order = received_.empty() || pn == received_.rbegin()->second + 1;
  const bool newest = received_.empty() || pn > received_.rbegin()->second;
  // Ranges are stored as [lo, hi] (inclusive).
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
  if (newest) largest_received_time_ = now;
  // Acknowledge every second packet, at once on reordering or loss,
  // otherwise within kMaxAckDelay.
  if (ack_pending_++ == 0) ack_deadline_ = now + kMaxAckDelay;
  if (ack_pending_ >= 2 || !in_order || !in_order_hint) ack_deadline_ = now;
  cv_.notify_all();
}

void ReliableTransport::HandleData(uint64_t pn, uint8_t ch, uint64_t off, std::span<const uint8_t> data,
                                   uint64_t now, Deliveries& deliver) {
  if (recv_bodies_.count(pn)) {  // a duplicate, or rebuilt already
    if (auto it = rebuilt_.find(pn); it != rebuilt_.end()) {
      // The original after all: it was only late, not lost.
      if (fec_recovered_ > 0) --fec_recovered_;
      rebuilt_.erase(it);
    }
    return;
  }
  MarkReceived(pn, now, true);
  // Kept to rebuild a neighbour from a parity.
  proto::ByteWriter bw;
  bw.u8(ch);
  bw.varint(off);
  std::vector<uint8_t> body = bw.take();
  body.insert(body.end(), data.begin(), data.end());
  recv_bodies_[pn] = std::move(body);
  while (recv_bodies_.size() > kMaxRecvBodies) recv_bodies_.erase(recv_bodies_.begin());

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

void ReliableTransport::TryRecover(uint64_t now, Deliveries& deliver) {
  // Rebuilding one packet can complete another parity's group: repeat.
  for (bool again = true; again;) {
    again = false;
    for (size_t i = 0; i < fec_pending_.size();) {
      const fec::Parity& par = fec_pending_[i].parity;
      // Older than what is kept: cannot tell, so no use.
      const bool known_range = !recv_bodies_.empty() && par.pns.front() >= recv_bodies_.begin()->first;
      size_t missing = 0;
      uint64_t which = 0;
      for (uint64_t pn : par.pns) {
        if (!recv_bodies_.count(pn)) {
          ++missing;
          which = pn;
        }
      }
      if (!known_range || missing == 0 || now - fec_pending_[i].time > 2'000'000 + 4 * srtt_) {
        fec_pending_.erase(fec_pending_.begin() + static_cast<std::ptrdiff_t>(i));
        continue;
      }
      if (missing == 1) {
        auto body = fec::Recover(par, which, recv_bodies_);
        fec_pending_.erase(fec_pending_.begin() + static_cast<std::ptrdiff_t>(i));
        if (body) {
          proto::ByteReader r(*body);
          uint8_t ch = 0;
          uint64_t off = 0;
          std::span<const uint8_t> data;
          if (r.u8(ch) && r.varint(off) && ch < kChannels && r.bytes(r.remaining(), data)) {
            const std::vector<uint8_t> copy(data.begin(), data.end());  // body is replaced below
            ++fec_recovered_;
            rebuilt_[which] = now;
            while (rebuilt_.size() > 1024 ||
                   (!rebuilt_.empty() && now - rebuilt_.begin()->second > 2'000'000 + 4 * srtt_)) {
              rebuilt_.erase(rebuilt_.begin());
            }
            HandleData(which, ch, off, copy, now, deliver);
            again = true;
          }
        }
        continue;
      }
      ++i;
    }
  }
}

void ReliableTransport::OnDatagram(std::span<const uint8_t> d) {
  if (d.empty()) return;
  std::vector<std::pair<int, std::vector<uint8_t>>> deliver;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // Read under the lock: a packet the sender thread sent while this one
    // waited for it would otherwise be newer than `now`, and acknowledging
    // it (only a broken or hostile peer can) would make the round trip
    // negative, wrapping to ~2^64 us.
    const uint64_t now = Now();
    if (closed_) return;
    if (!heard_peer_ && !opt_.client) {
      // Only the start of a connection may make its sender the peer: the
      // client's hello, or the first control bytes. Anything else (a port
      // scan, a late packet from the viewer before) must not take the place
      // of the next viewer, so its sender is forgotten, where the socket can.
      bool starts = d[0] == kHello;
      if (d[0] == kData) {
        proto::ByteReader h(d.subspan(1));
        uint64_t pn = 0, off = 0;
        uint8_t ch = 0;
        starts = h.varint(pn) && h.u8(ch) && h.varint(off) &&
                 ch == static_cast<uint8_t>(transport::Channel::kControl) && off == 0;
      }
      if (!starts && socket_->ForgetPeer()) return;
    }
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
      HandleData(pn, ch, off, data, now, deliver);
      TryRecover(now, deliver);
    } else if (d[0] == kFec) {
      uint64_t pn = 0;
      fec::Parity parity;
      if (!fec::Decode(d.subspan(1), &pn, &parity)) return;
      MarkReceived(pn, now, true);
      if (fec_pending_.size() >= kMaxPendingParities) fec_pending_.erase(fec_pending_.begin());
      fec_pending_.push_back({std::move(parity), now});
      TryRecover(now, deliver);
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
