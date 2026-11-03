#include "codec/hevc_bitstream.h"

namespace rdacore::codec {

std::vector<std::span<const uint8_t>> SplitAnnexB(std::span<const uint8_t> s) {
  std::vector<std::span<const uint8_t>> nals;
  const size_t n = s.size();
  // Finds the next 00 00 01 at or after i; returns n if none.
  auto find_start = [&](size_t i) {
    for (; i + 2 < n; ++i) {
      if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) return i;
    }
    return n;
  };

  size_t sc = find_start(0);
  while (sc < n) {
    const size_t begin = sc + 3;
    const size_t next = find_start(begin);
    size_t end = next;
    // Trailing zeros before the next start code belong to it (00 00 00 01)
    // or are trailing_zero_8bits; neither is part of this NAL.
    while (end > begin && s[end - 1] == 0) --end;
    if (end > begin) nals.push_back(s.subspan(begin, end - begin));
    sc = next;
  }
  return nals;
}

bool SplitLengthPrefixed(std::span<const uint8_t> data,
                         std::vector<std::span<const uint8_t>>& out) {
  size_t i = 0;
  while (i < data.size()) {
    if (data.size() - i < 4) return false;
    const uint32_t len = (static_cast<uint32_t>(data[i]) << 24) |
                         (static_cast<uint32_t>(data[i + 1]) << 16) |
                         (static_cast<uint32_t>(data[i + 2]) << 8) | data[i + 3];
    i += 4;
    if (len == 0 || len > data.size() - i) return false;
    out.push_back(data.subspan(i, len));
    i += len;
  }
  return true;
}

void AppendLengthPrefixed(std::span<const uint8_t> nal, std::vector<uint8_t>& out) {
  const auto len = static_cast<uint32_t>(nal.size());
  out.push_back(static_cast<uint8_t>(len >> 24));
  out.push_back(static_cast<uint8_t>(len >> 16));
  out.push_back(static_cast<uint8_t>(len >> 8));
  out.push_back(static_cast<uint8_t>(len));
  out.insert(out.end(), nal.begin(), nal.end());
}

void AppendAnnexB(std::span<const uint8_t> nal, std::vector<uint8_t>& out) {
  static constexpr uint8_t kStart[] = {0, 0, 0, 1};
  out.insert(out.end(), std::begin(kStart), std::end(kStart));
  out.insert(out.end(), nal.begin(), nal.end());
}

HevcAccessUnit AnnexBToAccessUnit(std::span<const uint8_t> annexb) {
  HevcAccessUnit au;
  for (auto nal : SplitAnnexB(annexb)) {
    const uint8_t type = HevcNalTypeOf(nal);
    if (type == kNalAud) continue;
    if (IsHevcParameterSet(type)) {
      au.parameter_sets.emplace_back(nal.begin(), nal.end());
      continue;
    }
    if (IsHevcIrap(type)) au.keyframe = true;
    AppendLengthPrefixed(nal, au.nal_data);
  }
  return au;
}

bool AccessUnitToAnnexB(const std::vector<std::vector<uint8_t>>& parameter_sets,
                        std::span<const uint8_t> nal_data, std::vector<uint8_t>& out) {
  std::vector<std::span<const uint8_t>> nals;
  if (!SplitLengthPrefixed(nal_data, nals)) return false;
  for (const auto& ps : parameter_sets) AppendAnnexB(ps, out);
  for (auto nal : nals) AppendAnnexB(nal, out);
  return true;
}

}  // namespace rdacore::codec
