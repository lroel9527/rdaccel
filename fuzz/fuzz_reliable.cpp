// The UDP reliability layer's datagram parser (data, acknowledgements,
// hellos), fed straight from the fuzzer.

#include <memory>

#include "fuzz_input.h"
#include "net/datagram.h"
#include "net/reliable_transport.h"

using namespace rdacore;

namespace {

class FeedSocket final : public net::DatagramSocket {
 public:
  bool Send(std::span<const uint8_t>) override { return true; }
  void SetReceiver(ReceiveFn fn) override { fn_ = std::move(fn); }
  void Close() override {}
  ReceiveFn fn_;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto sock = std::make_unique<FeedSocket>();
  FeedSocket* s = sock.get();
  net::ReliableTransport t(std::move(sock), net::ReliableOptions{.client = size > 0 && (data[0] & 1)});
  t.SetReceiver([](transport::Channel, std::span<const uint8_t>) {});
  t.Send(transport::Channel::kVideo, std::vector<uint8_t>(5000, 1));  // something to acknowledge
  fuzz::Pieces in(data + (size > 0), size - (size > 0));
  uint8_t ch;
  std::span<const uint8_t> chunk;
  while (in.Next(ch, chunk)) s->fn_(chunk);
  return 0;
}
