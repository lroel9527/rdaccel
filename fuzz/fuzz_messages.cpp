// Every wire message parser, through the stream parser, with arbitrary
// splits; parsed messages are re-encoded and must parse back identically.

#include <algorithm>

#include "fuzz_input.h"
#include "proto/messages.h"

using namespace rdacore;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  proto::StreamParser p;
  fuzz::Pieces in(data, size);
  uint8_t ch;
  std::span<const uint8_t> chunk;
  while (in.Next(ch, chunk)) {
    p.Append(chunk);
    while (auto m = p.Next()) {
      const auto bytes = proto::Encode(*m);
      proto::StreamParser again;
      again.Append(bytes);
      auto back = again.Next();
      if (!back || !(*back == *m)) __builtin_trap();
    }
    if (!p.error().empty()) break;
  }
  return 0;
}
