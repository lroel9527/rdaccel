#pragma once

// A Transport over an unreliable datagram link (UDP or the simulated link):
//
//   * Loss recovery: every datagram has a fresh packet number; the receiver
//     acknowledges ranges of them, and the sender resends the bytes of
//     packets it decides are lost (three later packets acknowledged, or
//     clearly older than one round trip, or no acknowledgement for a probe
//     timeout). A packet declared lost that is acknowledged after all (it
//     was only late: jitter, reordering) is counted as a spurious resend; the
//     rules stay quick, as waiting longer costs latency. Each channel is its own ordered byte stream, so a lost video
//     packet never holds up input, cursor or control bytes.
//   * Congestion control: a simplified BBR. The sender measures the delivery
//     rate from acknowledgements and the minimum round-trip time, paces
//     packets at about the measured bottleneck rate and caps bytes in flight
//     at two bandwidth-delay products. Unlike loss-based control it neither
//     fills the bottleneck buffer (latency stays near the base RTT) nor
//     backs off on random loss.
//   * Forward error correction (net/fec.h): on long, lossy paths a parity
//     packet follows every few data packets, and the end of every burst, so
//     the receiver rebuilds a single lost packet at once instead of waiting
//     a round trip for its resend. A rebuilt packet is acknowledged as
//     received; resends stay the fallback. Off where loss is rare or the
//     round trip short (a resend is quick there).
//   * Path statistics (GetStats) for the Host's adaptive bitrate.
//
// The wire format is private to this file pair; both ends must run it.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/datagram.h"
#include "net/fec.h"
#include "transport/transport.h"

namespace rdacore::net {

struct ReliableOptions {
  // The connecting side pings until it hears from the peer, so a listener
  // learns its address even if the client has nothing to send.
  bool client = false;
  // false: no pacing or window, send as fast as data comes (for comparisons).
  bool congestion_control = true;
  // Forward error correction: -1 automatic (round trip >= 40 ms and loss
  // seen), 0 off, 2..32 a parity packet every this many data packets.
  int fec = -1;
};

class ReliableTransport final : public transport::ITransport {
 public:
  ReliableTransport(std::unique_ptr<DatagramSocket> socket, ReliableOptions options = {});
  ~ReliableTransport() override;

  bool Send(transport::Channel ch, std::span<const uint8_t> bytes) override;
  void SetReceiver(ReceiveFn fn) override;
  void Close() override;
  bool GetStats(TransportStats* out) const override;
  bool PeerKnown() const override;
  std::unique_ptr<transport::ITransport> NextPeer(std::string* error) override;

 private:
  void Linger();
  static constexpr int kChannels = kChannelCount;

  struct SendStream {
    std::deque<uint8_t> buf;  // bytes from `base` on, kept until acknowledged
    uint64_t base = 0;
    uint64_t next = 0;                  // first byte never sent
    std::map<uint64_t, uint64_t> acked;  // [start, end) ranges past `base`
    std::map<uint64_t, uint64_t> resend;  // [start, end) ranges to send again
  };
  struct RecvStream {
    uint64_t next = 0;  // next byte to deliver
    std::map<uint64_t, std::vector<uint8_t>> pending;  // out-of-order data
  };
  struct SentPacket {
    uint8_t ch = 0;
    uint64_t offset = 0, len = 0;
    uint32_t size = 0;  // datagram bytes
    uint64_t time = 0;
    uint64_t delivered = 0, delivered_time = 0, first_sent_time = 0;
    bool app_limited = false;
    bool cwnd_limited = false;  // the window was full when it was sent
  };

  void Run();  // sender / timer thread
  void OnDatagram(std::span<const uint8_t> d);
  void HandleAck(std::span<const uint8_t> payload, uint64_t now);
  // Lock held for all of these.
  bool SendOne(uint64_t now);
  void SendAck(uint64_t now);
  void MarkLost(uint64_t pn, const SentPacket& p, uint64_t now);
  using Deliveries = std::vector<std::pair<int, std::vector<uint8_t>>>;
  // Receiving (lock held): a data packet, received or rebuilt.
  void MarkReceived(uint64_t pn, uint64_t now, bool in_order_hint);
  void HandleData(uint64_t pn, uint8_t ch, uint64_t off, std::span<const uint8_t> data, uint64_t now,
                  Deliveries& deliver);
  void TryRecover(uint64_t now, Deliveries& deliver);
  // Sending (lock held).
  void SendParity(uint64_t now);
  void UpdateFec(uint64_t now);
  void ReleaseAcked(SendStream& s);
  void OnRateSample(const SentPacket& p, uint64_t now);
  uint64_t QueuedBytes() const;
  uint64_t Cwnd() const;
  // The round trip shows a standing queue at the bottleneck.
  bool PathQueued() const;
  double PacingRate() const;  // bytes per microsecond
  void UpdatePacingRate();
  uint64_t Pto() const;

  std::unique_ptr<DatagramSocket> socket_;
  ReliableOptions opt_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool closed_ = false;
  bool heard_peer_ = false;
  uint64_t next_hello_ = 0;

  // Sending.
  SendStream out_[kChannels];
  uint64_t next_pn_ = 0;
  std::map<uint64_t, SentPacket> in_flight_;
  uint64_t bytes_in_flight_ = 0;
  uint64_t largest_acked_ = 0;
  bool any_acked_ = false;
  uint64_t next_send_time_ = 0;  // pacing
  uint64_t last_ack_time_ = 0;   // for the probe timeout
  int pto_count_ = 0;
  bool probe_due_ = false;  // a probe timeout fired: send one packet whatever the window
  uint64_t packets_sent_ = 0, packets_lost_ = 0, packets_spurious_ = 0;
  // Packets declared lost recently, by number: an acknowledgement for one
  // means its resend was spurious.
  struct LostPacket {
    uint64_t time = 0;  // when declared lost
    uint32_t size = 0;  // datagram bytes
  };
  std::map<uint64_t, LostPacket> lost_recent_;

  // Forward error correction, sending: the group being collected, its size
  // (0 = off), and what decides it.
  fec::Parity fec_group_;
  size_t fec_n_ = 0;
  // Parities may take at most kFecShare of the estimated bandwidth, and
  // nothing while the path is full (a queue at the bottleneck, or our own
  // data waiting): a token bucket in bytes.
  double fec_tokens_ = 0;
  uint64_t fec_tokens_time_ = 0;
  // Overflow guard: a parity after every burst can push a nearly full path
  // over (the bandwidth estimate cannot tell: ~1 Mbps on 1 and on 4 Mbps
  // paths while the sender is application limited). When the loss rate
  // climbs well above what it was when FEC went on, only full groups are
  // sent for a while.
  double fec_base_loss_ = 0;
  uint64_t fec_no_flush_until_ = 0;
  // Time the sender spent with data waiting (the window or the pacing held
  // it back): a sender busy that much has no room for parities, whatever
  // the round trip says (that queue is ours, not the network's).
  uint64_t backlogged_us_ = 0, fec_check_backlogged_ = 0;
  bool fec_busy_ = false;
  uint64_t fec_parity_sent_ = 0;
  uint64_t peer_fec_recovered_ = 0;  // the peer's count of our packets it rebuilt
  uint64_t fec_check_time_ = 0, fec_check_sent_ = 0, fec_check_lost_ = 0;
  double fec_loss_ = 0;            // recent loss rate, rebuilt packets included
  uint64_t fec_hold_until_ = 0;    // stays on until then once it went on
  // Receiving: recent data bodies (to rebuild from), parities still useful.
  std::map<uint64_t, std::vector<uint8_t>> recv_bodies_;
  struct PendingParity {
    fec::Parity parity;
    uint64_t time = 0;
  };
  std::vector<PendingParity> fec_pending_;
  uint64_t fec_recovered_ = 0;  // packets of the peer's we rebuilt (and that never came after all)
  // Rebuilt packets whose original may still come: on a jittery path a
  // parity can overtake a data packet that was only late. If the original
  // turns up, that was no loss and fec_recovered_ takes it back, or the
  // sender would see loss that is not there (and send parities too often).
  std::map<uint64_t, uint64_t> rebuilt_;  // packet number -> when rebuilt

  // RTT.
  uint64_t srtt_ = 0, rttvar_ = 0, latest_rtt_ = 0;
  uint64_t min_rtt_ = 0, min_rtt_time_ = 0;
  // The lowest round trip of the last two smoothed ones (100 ms to 1 s):
  // what says a queue stands. A single high sample is often just jitter;
  // a queue keeps every sample high.
  std::deque<std::pair<uint64_t, uint64_t>> recent_rtt_;  // {time, round trip}
  uint64_t RecentMinRtt() const;

  // BBR-style model.
  enum class Mode { kStartup, kDrain, kProbeBw } mode_ = Mode::kStartup;
  uint64_t delivered_ = 0, delivered_time_ = 0, first_sent_time_ = 0;
  uint64_t round_ = 0, next_round_delivered_ = 0;
  double bw_ = 0;  // bytes per microsecond, max over recent rounds
  double pacing_rate_ = 0;
  uint64_t backlog_since_ = UINT64_MAX;  // when the send queue last became non-empty  // bytes per microsecond; 0 = initial
  struct BwSample {
    uint64_t round;
    uint64_t time;  // when it was taken
    double bw;
  };
  std::deque<BwSample> bw_samples_;
  uint64_t queue_srtt_ = 0;   // the smoothed round trip when it began
  uint64_t queue_since_ = 0;  // when the round trip last rose above 2x its minimum
  double full_bw_ = 0;
  int full_bw_rounds_ = 0;
  int cycle_ = 0;
  uint64_t cycle_start_ = 0;

  // Receiving.
  RecvStream in_[kChannels];
  std::map<uint64_t, uint64_t> received_;  // packet numbers [lo, hi]
  int ack_pending_ = 0;
  uint64_t ack_deadline_ = 0;
  uint64_t largest_received_time_ = 0;

  std::mutex recv_mu_;
  std::condition_variable recv_cv_;
  ReceiveFn receiver_;

  std::thread worker_;
};

}  // namespace rdacore::net
