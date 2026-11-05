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
  // For a socket whose peer is the first sender: stops this one and returns
  // a socket on the same endpoint whose peer will be the next sender (what
  // the old peer had in flight is dropped). Null when the peer was given
  // (connected sockets, simulated links); this socket is then unchanged.
  virtual std::unique_ptr<DatagramSocket> NextPeer() { return nullptr; }
  // The same, in place, for a peer whose first datagram did not start a
  // connection: the next sender becomes the peer. Safe from the receive
  // callback. False (and nothing changes) when the peer was given.
  virtual bool ForgetPeer() { return false; }
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
// Binds and returns at once (IPv6 dual-stack where available); the first
// datagram's sender becomes the peer.
std::unique_ptr<DatagramSocket> BindUdpSocket(uint16_t port, std::string* error);
// Takes ownership of an application's UDP socket (closed with the result).
// Connected = that peer; unconnected = the first sender, as BindUdpSocket.
std::unique_ptr<DatagramSocket> UdpSocketFromFd(int fd, std::string* error);

}  // namespace rdacore::net
