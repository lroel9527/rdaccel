// The lossless layer decoder (deflate stream, region ops, palette tiles).

#include <string>

#include "codec/lossless.h"
#include "proto/messages.h"

using namespace rdacore;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 3) return 0;
  const int32_t w = 64 + (data[0] % 32) * 64;  // 64..2048
  const int32_t h = 64 + (data[1] % 32) * 64;
  const bool reset = data[2] & 1;
  codec::LosslessDecoder dec;
  codec::RegionOps ops;
  std::string err;
  std::span<const uint8_t> rest(data + 3, size - 3);
  // Two frames from the same stream: state carries over.
  const size_t half = rest.size() / 2;
  if (dec.Decode(rest.first(half), true, w, h, proto::kTileCacheSlots, ops, &err)) {
    dec.Decode(rest.subspan(half), reset, w, h, proto::kTileCacheSlots, ops, &err);
  }
  // The same with a context block (codec/context_tiles.h): the last quarter
  // of each half.
  codec::LosslessDecoder ctx;
  const size_t q = half / 4;
  if (ctx.Decode(rest.first(half - q), true, w, h, proto::kTileCacheSlots, ops, &err, rest.subspan(half - q, q))) {
    ctx.Decode(rest.subspan(half), reset, w, h, proto::kTileCacheSlots, ops, &err, rest.first(q));
  }
  return 0;
}
