// The connection layer, host and viewer role, on every channel.

#include <memory>

#include "fuzz_input.h"
#include "session/link.h"

using namespace rdacore;

namespace {

class NullTransport final : public transport::ITransport {
 public:
  bool Send(transport::Channel, std::span<const uint8_t>) override { return true; }
  void SetReceiver(ReceiveFn) override {}
  void Close() override {}
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) return 0;
  NullTransport t;
  session::Link::Handlers h;
  h.on_message = [](uint32_t, std::span<const uint8_t>) {};
  h.on_other = [](const proto::Message&) {};
  h.on_raw = [](transport::Channel, std::span<const uint8_t>) {};
  h.on_auth = [](std::span<const uint8_t> token) { return !token.empty() && token[0] == 'k'; };
  session::LinkOptions o;
  o.peer_timeout_ms = 0;
  session::Link link(data[0] & 1 ? session::Link::Role::kHost : session::Link::Role::kViewer, &t, o,
                     std::move(h));
  fuzz::Pieces in(data + 1, size - 1);
  uint8_t ch;
  std::span<const uint8_t> chunk;
  while (in.Next(ch, chunk)) link.OnReceive(static_cast<transport::Channel>(ch % kChannelCount), chunk);
  link.Shutdown();
  return 0;
}
