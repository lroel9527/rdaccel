// The HEVC bitstream helpers that look at encoder and network data.

#include "codec/hevc_bitstream.h"

using namespace rdacore;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::span<const uint8_t> in(data, size);
  for (auto nal : codec::SplitAnnexB(in)) (void)codec::HevcNalTypeOf(nal);
  auto au = codec::AnnexBToAccessUnit(in);
  std::vector<uint8_t> out;
  codec::AccessUnitToAnnexB(au.parameter_sets, au.nal_data, out);
  std::vector<std::span<const uint8_t>> nals;
  codec::SplitLengthPrefixed(in, nals);
  return 0;
}
