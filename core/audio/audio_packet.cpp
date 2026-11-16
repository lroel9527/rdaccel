#include "audio/audio_packet.h"

namespace rdacore::audio {

bool EncodePacket(const Packet& p, std::vector<uint8_t>& out) {
  out.clear();
  if (p.main.empty() || (p.flags & ~(kFlagClock | kFlagRed | kFlagEnd))) return false;
  const bool red = p.flags & kFlagRed;
  if (red && (p.red.empty() || p.red.size() > 0xFFFF)) return false;
  const size_t size = kPacketHeader + (p.clock() ? 8 : 0) + (red ? 2 + p.red.size() : 0) + p.main.size();
  if (size > kMaxPacket) return false;
  out.reserve(size);
  out.push_back(kPacketKind);
  out.push_back(p.flags);
  out.push_back(p.spurt);
  out.push_back(static_cast<uint8_t>(p.seq));
  out.push_back(static_cast<uint8_t>(p.seq >> 8));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(p.ts >> (8 * i)));
  if (p.clock()) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(p.time_us >> (8 * i)));
  }
  if (red) {
    out.push_back(static_cast<uint8_t>(p.red.size()));
    out.push_back(static_cast<uint8_t>(p.red.size() >> 8));
    out.insert(out.end(), p.red.begin(), p.red.end());
  }
  out.insert(out.end(), p.main.begin(), p.main.end());
  return true;
}

std::optional<Packet> ParsePacket(std::span<const uint8_t> b) {
  if (b.size() < kPacketHeader + 1 || b.size() > kMaxPacket || b[0] != kPacketKind) return std::nullopt;
  Packet p;
  p.flags = b[1];
  if (p.flags & ~(kFlagClock | kFlagRed | kFlagEnd)) return std::nullopt;
  p.spurt = b[2];
  p.seq = static_cast<uint16_t>(b[3] | (b[4] << 8));
  p.ts = 0;
  for (int i = 0; i < 4; ++i) p.ts |= static_cast<uint32_t>(b[5 + i]) << (8 * i);
  size_t pos = kPacketHeader;
  if (p.clock()) {
    if (b.size() < pos + 8) return std::nullopt;
    for (int i = 0; i < 8; ++i) p.time_us |= static_cast<uint64_t>(b[pos + i]) << (8 * i);
    pos += 8;
  }
  if (p.flags & kFlagRed) {
    if (b.size() < pos + 2) return std::nullopt;
    const size_t len = b[pos] | (static_cast<size_t>(b[pos + 1]) << 8);
    pos += 2;
    if (len == 0 || b.size() < pos + len) return std::nullopt;
    p.red = b.subspan(pos, len);
    pos += len;
  }
  if (pos >= b.size()) return std::nullopt;  // no main frame
  p.main = b.subspan(pos);
  return p;
}

int64_t TsUnwrapper::Unwrap(uint32_t ts) {
  if (!have_) {
    have_ = true;
    last_ = ts;
    return last_;
  }
  const auto delta = static_cast<int32_t>(ts - static_cast<uint32_t>(last_));
  last_ += delta;
  return last_;
}

}  // namespace rdacore::audio
