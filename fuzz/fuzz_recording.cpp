// .rdstream playback: a recording file as rdaccel_play and
// rda_viewer_feed_recording() read it, through the real HEVC decoder of
// the platform. The corpus in fuzz/corpus/recording/ holds real recordings.

#include <cstring>
#include <memory>

#include "io/stream_file.h"
#include "platform.h"
#include "session/viewer_session.h"

using namespace rdacore;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // The 8-byte magic, as ReadStreamFile checks it.
  if (size < sizeof(io::kStreamMagic) || std::memcmp(data, io::kStreamMagic, sizeof(io::kStreamMagic)) != 0) {
    return -1;  // not a recording: keep it out of the corpus
  }
  data += sizeof(io::kStreamMagic);
  size -= sizeof(io::kStreamMagic);
  auto dec = platform::CreateHevcDecoder(nullptr);
  if (!dec) return 0;
  session::ViewerSession v(nullptr, std::move(dec));
  v.SetDecoderFactory([] { return platform::CreateHevcDecoder(nullptr); });
  v.SetFrameHandler([](const tile::ImageView& img, const std::vector<Rect>&, uint64_t, uint64_t) {
    volatile uint8_t sink = img.data[0];
    (void)sink;
  });
  v.SetCursorHandler([](const CursorState&) {});
  // Played in two pieces, split where the first byte says: the parser must
  // resume mid-message.
  const size_t cut = size ? size * data[0] / 256 : 0;
  v.FeedVideoBytes({data, cut});
  v.FeedVideoBytes({data + cut, size - cut});
  return 0;
}
