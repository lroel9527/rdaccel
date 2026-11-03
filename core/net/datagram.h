#pragma once

// Unreliable datagram links underneath net/reliable_transport.h:
//   * UDP sockets (macOS and Linux), for real networks;
//   * a simulated link with a bandwidth limit, delay, jitter, random loss and
//     a bounded bottleneck queue, for repeatable tests and benchmarks.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rdacore::net {

class DatagramSocket {
 public:
  using ReceiveFn = std::function<void(std::span<const uint8_t>)>;
  virtual ~DatagramSocket() = default;
  // Never blocks; a datagram may be silently lost. False once closed.
  virtual bool Send(std::span<const uint8_t> datagram) = 0;
  // Called on a socket-owned thread, one datagram at a time. Must be set
  // once, before the first datagram is expected; earlier ones are dropped.
  virtual void SetReceiver(ReceiveFn fn) = 0;
  // Stops the receive thread (waits for a running callback to return).
  virtual void Close() = 0;
};

// Largest datagram the transport sends; small enough for any path MTU
// including tunnels (IPv6 minimum is 1280 bytes).
inline constexpr size_t kMaxDatagram = 1200;

struct LinkConditions {
  int64_t bandwidth_bps = 0;  // 0 = unlimited
  int32_t delay_ms = 0;       // one way
  int32_t jitter_ms = 0;      // extra uniform random delay (reorders packets)
  double loss = 0;            // random loss probability per datagram
  int32_t queue_ms = 200;     // bottleneck buffer, in time at `bandwidth_bps`
  // Bandwidth changes over time: {milliseconds since creation, bps}.
  std::vector<std::pair<int64_t, int64_t>> bandwidth_schedule = {};
  uint32_t seed = 1;
};

struct LinkStats {
  uint64_t sent = 0;
  uint64_t lost = 0;     // random loss
  uint64_t dropped = 0;  // bottleneck queue overflow
};

// Two connected endpoints; `a_to_b` applies to what the first one sends.
std::pair<std::unique_ptr<DatagramSocket>, std::unique_ptr<DatagramSocket>> MakeSimulatedLink(
    const LinkConditions& a_to_b, const LinkConditions& b_to_a);

// UDP. Listen waits until the first datagram arrives and then talks only to
// its sender (that first datagram is delivered too).
std::unique_ptr<DatagramSocket> ListenUdpSocket(uint16_t port, std::string* error);
std::unique_ptr<DatagramSocket> ConnectUdpSocket(const std::string& host, uint16_t port,
                                                 std::string* error);

}  // namespace rdacore::net
