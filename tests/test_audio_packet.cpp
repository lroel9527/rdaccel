// The sound packet format (core/audio/audio_packet.h).

#include <gtest/gtest.h>

#include <vector>

#include "audio/audio_packet.h"

using namespace rdacore::audio;

namespace {

std::vector<uint8_t> Bytes(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 13);
  return v;
}

}  // namespace

TEST(AudioPacket, RoundTripsEveryField) {
  const auto main = Bytes(300, 1), red = Bytes(280, 2);
  for (uint8_t flags = 0; flags < 8; ++flags) {
    Packet p;
    p.flags = flags;
    p.spurt = 200;
    p.seq = 0xBEEF;
    p.ts = 0xFFFFFF00u;
    p.time_us = 0x0123456789ABCDEFull;
    if (flags & kFlagRed) p.red = red;
    p.main = main;
    std::vector<uint8_t> out;
    ASSERT_TRUE(EncodePacket(p, out));
    EXPECT_EQ(out.size(), kPacketHeader + (flags & kFlagClock ? 8 : 0) + (flags & kFlagRed ? 2 + red.size() : 0) +
                              main.size());
    const auto q = ParsePacket(out);
    ASSERT_TRUE(q);
    EXPECT_EQ(q->flags, flags);
    EXPECT_EQ(q->spurt, 200);
    EXPECT_EQ(q->seq, 0xBEEF);
    EXPECT_EQ(q->ts, 0xFFFFFF00u);
    EXPECT_EQ(q->time_us, flags & kFlagClock ? p.time_us : 0u);
    EXPECT_EQ(std::vector<uint8_t>(q->main.begin(), q->main.end()), main);
    if (flags & kFlagRed) EXPECT_EQ(std::vector<uint8_t>(q->red.begin(), q->red.end()), red);
    EXPECT_EQ(q->end(), (flags & kFlagEnd) != 0);
  }
}

TEST(AudioPacket, LittleEndianOnTheWire) {
  const auto main = Bytes(4, 9);
  Packet p;
  p.flags = kFlagClock;
  p.spurt = 7;
  p.seq = 0x0102;
  p.ts = 0x03040506;
  p.time_us = 0x0708090A0B0C0D0Eull;
  p.main = main;
  std::vector<uint8_t> out;
  ASSERT_TRUE(EncodePacket(p, out));
  const std::vector<uint8_t> head = {1, kFlagClock, 7, 0x02, 0x01, 0x06, 0x05, 0x04, 0x03,
                                     0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07};
  EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.begin() + 17), head);
}

TEST(AudioPacket, RejectsWhatIsMalformed) {
  const auto main = Bytes(100, 3), red = Bytes(90, 4);
  Packet p;
  p.flags = kFlagClock | kFlagRed;
  p.red = red;
  p.main = main;
  std::vector<uint8_t> good;
  ASSERT_TRUE(EncodePacket(p, good));
  // Every truncation that cuts into the header, the clock or the copy.
  for (size_t n = 0; n <= kPacketHeader + 8 + 2 + red.size(); ++n) {
    EXPECT_FALSE(ParsePacket(std::span<const uint8_t>(good).first(n))) << n;
  }
  // Reserved flag bits, another kind.
  for (uint8_t bit = 3; bit < 8; ++bit) {
    auto bad = good;
    bad[1] |= static_cast<uint8_t>(1u << bit);
    EXPECT_FALSE(ParsePacket(bad));
  }
  auto other = good;
  other[0] = 2;
  EXPECT_FALSE(ParsePacket(other));
  // A copy longer than the packet, or empty.
  auto longer = good;
  longer[kPacketHeader + 8] = 0xFF;
  longer[kPacketHeader + 9] = 0xFF;
  EXPECT_FALSE(ParsePacket(longer));
  auto empty_red = good;
  empty_red[kPacketHeader + 8] = 0;
  empty_red[kPacketHeader + 9] = 0;
  EXPECT_FALSE(ParsePacket(empty_red));
  // Larger than a datagram.
  std::vector<uint8_t> huge(kMaxPacket + 1, 0);
  huge[0] = kPacketKind;
  EXPECT_FALSE(ParsePacket(huge));
  // And the encoder refuses what would not fit or has no frame.
  const auto big = Bytes(kMaxPacket, 5);
  Packet q;
  q.main = big;
  std::vector<uint8_t> out;
  EXPECT_FALSE(EncodePacket(q, out));
  Packet none;
  EXPECT_FALSE(EncodePacket(none, out));
  Packet red_without_copy;
  red_without_copy.flags = kFlagRed;
  red_without_copy.main = main;
  EXPECT_FALSE(EncodePacket(red_without_copy, out));
}

TEST(AudioPacket, TsUnwrapsAcrossTheWrap) {
  TsUnwrapper u;
  EXPECT_EQ(u.Unwrap(0xFFFFFE00u), 0xFFFFFE00ll);
  EXPECT_EQ(u.Unwrap(0xFFFFFFE0u), 0xFFFFFFE0ll);
  EXPECT_EQ(u.Unwrap(0x00000100u), 0x100000100ll);  // wrapped: onwards
  EXPECT_EQ(u.Unwrap(0xFFFFFFF0u), 0xFFFFFFF0ll);   // a late one from before the wrap
  EXPECT_EQ(u.Unwrap(0x00000200u), 0x100000200ll);
  // Far into the next lap, in steps of 20 ms.
  int64_t expect = 0x100000200ll;
  uint32_t ts = 0x200;
  for (int i = 0; i < 300'000; ++i) {
    ts += 960;
    expect += 960;
    ASSERT_EQ(u.Unwrap(ts), expect);
  }
}
