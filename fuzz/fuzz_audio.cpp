// The sound receiver end to end: any datagram into Receiver::OnPacket, then
// Read as a player would call it (the packets' own Opus data, however
// malformed, goes through the decoder, the jitter buffer and the resampler).
// Input: [tag][length: u16 little-endian][that many bytes]..., a datagram
// each; the tag says how much time passes and how much is read after it.

#include <algorithm>
#include <vector>

#include "audio/receiver.h"

using namespace rdacore;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  audio::Receiver r;
  uint64_t now = 1'000'000;
  std::vector<float> out(512 * 2);
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  while (end - p >= 3) {
    const uint8_t tag = p[0];
    size_t n = p[1] | (static_cast<size_t>(p[2]) << 8);
    p += 3;
    n = std::min<size_t>(n, static_cast<size_t>(end - p));
    now += static_cast<uint64_t>(tag) * 500;
    r.OnPacket({p, n}, now);
    p += n;
    if (tag & 1) r.NoteFrameShown(now - 30'000, now);
    r.Read(out.data(), 1 + (tag % 4) * 128, 0, now);
  }
  return 0;
}
