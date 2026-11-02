#pragma once

// TCP transport over POSIX sockets (macOS and Linux). All channels share one
// connection; each chunk is framed as [channel: u8][length: u32 LE][bytes].
// This is the M0 cross-machine transport. Because TCP is one ordered stream,
// a large video frame delays input on the same connection; the QUIC transport
// planned for later removes that head-of-line blocking.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "transport/transport.h"

namespace rdaccel::transport {

class TcpTransport final : public ITransport {
 public:
  // Blocks until a peer connects to `port` on all interfaces. Returns null and
  // fills `error` on failure.
  static std::unique_ptr<TcpTransport> Listen(uint16_t port, std::string* error);
  static std::unique_ptr<TcpTransport> Connect(const std::string& host, uint16_t port,
                                               std::string* error);

  ~TcpTransport() override;

  bool Send(Channel ch, std::span<const uint8_t> bytes) override;
  void SetReceiver(ReceiveFn fn) override;
  void Close() override;

 private:
  explicit TcpTransport(int fd);
  void ReadLoop();

  int fd_;
  std::atomic<bool> closed_{false};
  std::mutex send_mu_;
  std::mutex recv_mu_;
  std::condition_variable recv_cv_;
  ReceiveFn receiver_;
  std::thread reader_;
};

}  // namespace rdaccel::transport
