#pragma once

// Connection state shared by Host and Viewer (see session/link.h).

#include <cstdint>

namespace rdacore {

enum class ConnectionState { kConnecting = 0, kConnected = 1, kDisconnected = 2 };

enum class ConnectionError {
  kNone = 0,
  kClosed,    // the peer said goodbye, or the transport closed
  kTimeout,   // nothing heard from the peer for peer_timeout_ms
  kProtocol,  // incompatible version or malformed data
  kRejected,  // the host's auth hook refused the viewer
};

// Application message types: below kFirstUserMessage the library defines
// them; applications use kFirstUserMessage and above.
inline constexpr uint32_t kClipboardTextMessage = 1;  // UTF-8 text
inline constexpr uint32_t kFirstUserMessage = 0x10000;

}  // namespace rdacore
