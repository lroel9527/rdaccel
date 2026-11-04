#pragma once

// A Transport over an unreliable datagram link (UDP or the simulated link):
//
//   * Loss recovery: every datagram has a fresh packet number; the receiver
//     acknowledges ranges of them, and the sender resends the bytes of
//     packets it decides are lost (three later packets acknowledged, or
//     clearly older than one round trip, or no acknowledgement for a probe
//     timeout). Each channel is its own ordered byte stream, so a lost video
//     packet never holds up input, cursor or control bytes.
//   * Congestion control: a simplified BBR. The sender measures the delivery
//     rate from acknowledgements and the minimum round-trip time, paces
//     packets at about the measured bottleneck rate and caps bytes in flight
//     at two bandwidth-delay products. Unlike loss-based control it neither
//     fills the bottleneck buffer (latency stays near the base RTT) nor
//     backs off on random loss.
//   * Path statistics (GetStats) for the Host's adaptive bitrate.
//
// The wire format is private to this file pair; both ends must run it.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "net/datagram.h"
#include "transport/transport.h"

namespace rdacore::net {

struct ReliableOptions {
  // The connecting side pings until it hears from the peer, so a listener
  // learns its address even if the client has nothing to send.
  bool client = false;
  // false: no pacing or window, send as fast as data comes (for comparisons).
  bool congestion_control = true;
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

 private:
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
  void MarkLost(uint64_t pn, const SentPacket& p);
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
  uint64_t packets_sent_ = 0, packets_lost_ = 0;

  // RTT.
  uint64_t srtt_ = 0, rttvar_ = 0, latest_rtt_ = 0;
  uint64_t min_rtt_ = 0, min_rtt_time_ = 0;

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
