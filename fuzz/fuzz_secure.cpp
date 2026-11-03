// The encrypted transport's receive side: the peer hello, record framing,
// alerts and authentication failures.

#include <memory>

#include "fuzz_input.h"
#include "transport/secure_transport.h"

using namespace rdacore;

namespace {

// Hands the fuzzer's bytes to whoever set the receiver.
class FeedTransport final : public transport::ITransport {
 public:
  bool Send(transport::Channel, std::span<const uint8_t>) override { return true; }
  void SetReceiver(ReceiveFn fn) override { fn_ = std::move(fn); }
  void Close() override {}
  ReceiveFn fn_;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto feed = std::make_unique<FeedTransport>();
  FeedTransport* f = feed.get();
  const std::array<uint8_t, 32> key{};
  transport::SecureTransport s(std::move(feed), size > 0 && (data[0] & 1), key);
  s.SetErrorHandler([](ConnectionError, const std::string&) {});
  s.SetReceiver([](transport::Channel, std::span<const uint8_t>) {});
  // Usually start with a well-formed hello so the record parser is reached.
  if (size > 0 && (data[0] & 2)) {
    uint8_t hello[37] = {'R', 'D', 'A', 'S', 1};
    f->fn_(transport::Channel::kControl, hello);
  }
  fuzz::Pieces in(data + (size > 0), size - (size > 0));
  uint8_t ch;
  std::span<const uint8_t> chunk;
  while (in.Next(ch, chunk)) f->fn_(static_cast<transport::Channel>(ch % kChannelCount), chunk);
  return 0;
}
