#pragma once

// Transport abstraction. M0 ships an in-process pipe (for single-machine
// loopback) and a TCP transport (for Mac <-> Linux). QUIC via Network.framework
// replaces TCP on macOS later; the interface stays the same.

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace rdaccel::transport {

enum class Channel : uint8_t {
  kControl = 0,
  kInput = 1,
  kSurface = 2,
  kVideo = 3,
  kCursor = 4,
};

class ITransport {
 public:
  using ReceiveFn = std::function<void(Channel, std::span<const uint8_t>)>;

  virtual ~ITransport() = default;

  // Queues bytes for sending. Thread-safe. Returns false if the connection is
  // closed.
  virtual bool Send(Channel ch, std::span<const uint8_t> bytes) = 0;

  // Sets the receive callback. It runs on a transport-owned thread and must
  // not block for long. Bytes on one channel arrive in order, possibly split
  // or merged differently than they were sent.
  virtual void SetReceiver(ReceiveFn fn) = 0;

  virtual void Close() = 0;
};

}  // namespace rdaccel::transport
