#pragma once

// How a Host and a Viewer talk to each other.
//
// The library ships an in-process pipe (both ends in one process), TCP, and
// UDP with loss recovery and congestion control (the one to use on real
// networks). Applications can plug in their own (QUIC, WebRTC data channels,
// a relay server, ...) by implementing Transport. None of them encrypts or
// authenticates; across the internet, run inside a VPN or your own secure
// transport. See docs/integration.md.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rdacore {

// Logical channels. A transport may map them to separate streams (so a large
// video frame does not delay input) or multiplex them on one connection.
enum class Channel : uint8_t {
  kControl = 0,
  kInput = 1,
  kSurface = 2,
  kVideo = 3,
  kCursor = 4,
  kMessage = 5,  // application messages (clipboard, ...): reliable, ordered
};

inline constexpr int kChannelCount = 6;

// What a transport knows about the path. The Host uses it to adapt its
// bitrate and to skip frames instead of letting a queue build up.
struct TransportStats {
  int64_t bandwidth_bps = 0;   // estimated deliverable send rate; 0 = unknown
  uint32_t rtt_us = 0;         // smoothed round-trip time; 0 = unknown
  uint64_t queued_bytes = 0;   // accepted by Send() but not yet on the wire
  uint64_t bytes_in_flight = 0;
  uint64_t packets_sent = 0;
  uint64_t packets_lost = 0;   // detected lost (and resent)
};

class Transport {
 public:
  using ReceiveFn = std::function<void(Channel, std::span<const uint8_t>)>;

  virtual ~Transport() = default;

  // Queues bytes for sending. Must be thread-safe. Returns false if the
  // connection is closed.
  virtual bool Send(Channel ch, std::span<const uint8_t> bytes) = 0;

  // Sets the receive callback. It runs on a transport-owned thread. Bytes on
  // one channel must arrive in order, but may be split or merged differently
  // than they were sent. Bytes that arrive before a receiver is set must be
  // held, not dropped.
  virtual void SetReceiver(ReceiveFn fn) = 0;

  // Stops sending and receiving. The destructor must also stop any thread the
  // transport owns and wait for a running receive callback to return.
  virtual void Close() = 0;

  // Fills `out` and returns true if this transport measures the path. The
  // default (and the in-process pipe and TCP) returns false; the Host then
  // sends at the configured bitrate.
  // False while a listening transport does not know its peer yet (nothing
  // sent would arrive); the Host then holds frames instead of encoding them.
  virtual bool PeerKnown() const { return true; }

  virtual bool GetStats(TransportStats* out) const {
    (void)out;
    return false;
  }
};

// Two connected in-process endpoints: what one sends, the other receives.
std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>> CreateLocalPipe();

// TCP. Both return null and set `error` on failure.
std::unique_ptr<Transport> ConnectTcp(const std::string& host, uint16_t port,
                                      std::string* error = nullptr);
// Host side without blocking: listens on `port` (IPv4 and IPv6) and returns
// at once; the first viewer to connect becomes the peer.
std::unique_ptr<Transport> BindTcp(uint16_t port, std::string* error = nullptr);

// UDP with its own loss recovery (selective acknowledgements and resends,
// one ordered stream per channel, so lost video never delays input) and
// BBR-style congestion control that keeps queues short. Reports TransportStats,
// so the Host adapts its bitrate. Both ends must use it.
std::unique_ptr<Transport> ConnectUdp(const std::string& host, uint16_t port,
                                      std::string* error = nullptr);
// Host side without blocking: binds `port` (IPv4 and IPv6) and returns at
// once; the first viewer that sends becomes the peer. The Host's state
// handler tells when it has connected.
std::unique_ptr<Transport> BindUdp(uint16_t port, std::string* error = nullptr);
// An application's UDP socket (after NAT traversal, a socket passed in by a
// launcher, ...). Taken over only on success. A connected socket talks to
// its peer; an unconnected one to the first sender (host side only).
// `viewer_side` = the end that creates the Viewer (it must be connected).
std::unique_ptr<Transport> UdpFromFd(int fd, bool viewer_side, std::string* error = nullptr);

// A network path for tests and benchmarks.
struct NetworkConditions {
  int64_t bandwidth_bps = 0;  // 0 = unlimited
  int32_t delay_ms = 0;       // one way
  int32_t jitter_ms = 0;      // extra random delay, reorders packets
  double loss = 0;            // random loss probability per packet
  int32_t queue_ms = 200;     // bottleneck buffer at `bandwidth_bps`
  // Bandwidth changes: {milliseconds after creation, bps}.
  std::vector<std::pair<int64_t, int64_t>> bandwidth_schedule = {};
  uint32_t seed = 1;
};

// Two connected endpoints running the UDP transport's protocol over an
// in-process simulated path (no sockets). `congestion_control` = false sends
// as fast as data comes, for comparison.
std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>> CreateSimulatedNetwork(
    const NetworkConditions& host_to_viewer, const NetworkConditions& viewer_to_host,
    bool congestion_control = true);

}  // namespace rdacore
