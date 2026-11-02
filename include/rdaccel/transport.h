#pragma once

// How a Host and a Viewer talk to each other.
//
// The library ships an in-process pipe (both ends in one process) and a TCP
// transport. Applications can plug in their own (QUIC, WebRTC data channels,
// a relay server, ...) by implementing Transport.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace rdaccel {

// Logical channels. A transport may map them to separate streams (so a large
// video frame does not delay input) or multiplex them on one connection.
enum class Channel : uint8_t {
  kControl = 0,
  kInput = 1,
  kSurface = 2,
  kVideo = 3,
  kCursor = 4,
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
};

// Two connected in-process endpoints: what one sends, the other receives.
std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>> CreateLocalPipe();

// TCP. Listen blocks until one peer connects. Both return null and set
// `error` on failure.
std::unique_ptr<Transport> ListenTcp(uint16_t port, std::string* error = nullptr);
std::unique_ptr<Transport> ConnectTcp(const std::string& host, uint16_t port,
                                      std::string* error = nullptr);

}  // namespace rdaccel
