#include "transport/local_pipe.h"

#include <vector>

namespace rdacore::transport {

struct LocalPipeEndpoint::Shared {
  struct Item {
    Channel ch;
    std::vector<uint8_t> bytes;
    bool datagram = false;
  };

  std::mutex mu;
  std::condition_variable cv;
  std::deque<Item> queue[2];
  ITransport::ReceiveFn receiver[2];
  ITransport::DatagramFn datagram_receiver[2];
  bool closed = false;
};

std::pair<std::unique_ptr<LocalPipeEndpoint>, std::unique_ptr<LocalPipeEndpoint>> MakeLocalPipe() {
  auto shared = std::make_shared<LocalPipeEndpoint::Shared>();
  std::unique_ptr<LocalPipeEndpoint> a(new LocalPipeEndpoint(shared, 0));
  std::unique_ptr<LocalPipeEndpoint> b(new LocalPipeEndpoint(shared, 1));
  return {std::move(a), std::move(b)};
}

LocalPipeEndpoint::LocalPipeEndpoint(std::shared_ptr<Shared> shared, int self)
    : shared_(std::move(shared)), self_(self), worker_([this] { Run(); }) {}

LocalPipeEndpoint::~LocalPipeEndpoint() {
  Close();
  if (worker_.joinable()) worker_.join();
}

bool LocalPipeEndpoint::Send(Channel ch, std::span<const uint8_t> bytes) {
  {
    std::lock_guard<std::mutex> lock(shared_->mu);
    if (shared_->closed) return false;
    shared_->queue[1 - self_].push_back({ch, {bytes.begin(), bytes.end()}});
  }
  shared_->cv.notify_all();
  return true;
}

bool LocalPipeEndpoint::SendDatagram(std::span<const uint8_t> bytes) {
  if (bytes.size() > kMaxDatagramPayload) return false;
  {
    std::lock_guard<std::mutex> lock(shared_->mu);
    if (shared_->closed) return false;
    shared_->queue[1 - self_].push_back({Channel::kControl, {bytes.begin(), bytes.end()}, true});
  }
  shared_->cv.notify_all();
  return true;
}

void LocalPipeEndpoint::SetDatagramReceiver(DatagramFn fn) {
  std::lock_guard<std::mutex> lock(shared_->mu);
  shared_->datagram_receiver[self_] = std::move(fn);
}

void LocalPipeEndpoint::SetReceiver(ReceiveFn fn) {
  std::lock_guard<std::mutex> lock(shared_->mu);
  shared_->receiver[self_] = std::move(fn);
  shared_->cv.notify_all();
}

void LocalPipeEndpoint::Close() {
  {
    std::lock_guard<std::mutex> lock(shared_->mu);
    shared_->closed = true;
  }
  shared_->cv.notify_all();
}

void LocalPipeEndpoint::Run() {
  auto& q = shared_->queue[self_];
  for (;;) {
    Shared::Item item;
    ReceiveFn fn;
    DatagramFn dfn;
    {
      std::unique_lock<std::mutex> lock(shared_->mu);
      // Hold items until a receiver is installed, so nothing is dropped
      // during setup (datagrams are not held: without a receiver they go).
      shared_->cv.wait(lock, [&] {
        return shared_->closed || (!q.empty() && (q.front().datagram || shared_->receiver[self_]));
      });
      if (shared_->closed) return;
      item = std::move(q.front());
      q.pop_front();
      fn = shared_->receiver[self_];
      dfn = shared_->datagram_receiver[self_];
    }
    if (item.datagram) {
      if (dfn) dfn(item.bytes);
      continue;
    }
    fn(item.ch, item.bytes);
  }
}

}  // namespace rdacore::transport
