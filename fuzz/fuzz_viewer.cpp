// The viewer session end to end: frame assembly, lossless tiles, moves,
// cache loads, region streams and cursor messages on every channel, with a
// fake decoder so pictures "decode" to a flat image of some size.

#include <memory>

#include "fuzz_input.h"
#include "session/viewer_session.h"

using namespace rdacore;

namespace {

class FakeDecoder final : public codec::IHevcDecoder {
 public:
  std::string name() const override { return "fake"; }
  const std::string& last_error() const override { return err_; }
  bool Decode(const std::vector<std::vector<uint8_t>>&, const std::vector<uint8_t>& nal, uint64_t pts,
              const codec::DecodedFrameFn& on_frame) override {
    if (nal.size() < 2 || !(nal[0] & 1)) return false;
    const int32_t w = 16 * (1 + nal[1] % 16), h = 16 * (1 + nal[0] % 16);
    px_.assign(static_cast<size_t>(w) * h * 4, 0x80);
    on_frame({px_.data(), w, h, w * 4}, pts);
    return true;
  }

 private:
  std::string err_;
  std::vector<uint8_t> px_;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  session::ViewerSession v(nullptr, std::make_unique<FakeDecoder>());
  v.SetDecoderFactory([] { return std::make_unique<FakeDecoder>(); });
  v.SetFrameHandler([](const tile::ImageView& img, const std::vector<Rect>&, uint64_t, uint64_t) {
    volatile uint8_t sink = img.data[0];
    (void)sink;
  });
  v.SetCursorHandler([](const CursorState&) {});
  fuzz::Pieces in(data, size);
  uint8_t ch;
  std::span<const uint8_t> chunk;
  while (in.Next(ch, chunk)) v.OnReceive(static_cast<transport::Channel>(ch % kChannelCount), chunk);
  return 0;
}
