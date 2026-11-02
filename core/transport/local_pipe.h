#pragma once

// In-process transport: two connected endpoints that hand bytes to each other
// through a queue drained by a worker thread. Used for single-machine loopback
// demos and for tests.

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "transport/transport.h"

namespace rdaccel::transport {

class LocalPipeEndpoint;

// Creates two endpoints; what one sends, the other receives.
std::pair<std::unique_ptr<LocalPipeEndpoint>, std::unique_ptr<LocalPipeEndpoint>> MakeLocalPipe();

class LocalPipeEndpoint final : public ITransport {
 public:
  ~LocalPipeEndpoint() override;

  bool Send(Channel ch, std::span<const uint8_t> bytes) override;
  void SetReceiver(ReceiveFn fn) override;
  void Close() override;

 private:
  friend std::pair<std::unique_ptr<LocalPipeEndpoint>, std::unique_ptr<LocalPipeEndpoint>>
  MakeLocalPipe();

  // State shared by the two endpoints; queue[i] holds bytes for endpoint i.
  struct Shared;

  LocalPipeEndpoint(std::shared_ptr<Shared> shared, int self);
  void Run();

  std::shared_ptr<Shared> shared_;
  int self_;
  std::thread worker_;
};

}  // namespace rdaccel::transport
