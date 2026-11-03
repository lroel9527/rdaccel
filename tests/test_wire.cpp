#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "proto/wire.h"

using rdacore::proto::ByteReader;
using rdacore::proto::ByteWriter;

TEST(Wire, VarintRoundTrip) {
  const std::vector<uint64_t> values = {0, 1, 127, 128, 300, 16383, 16384,
                                        std::numeric_limits<uint32_t>::max(),
                                        std::numeric_limits<uint64_t>::max()};
  ByteWriter w;
  for (uint64_t v : values) w.varint(v);
  ByteReader r(w.data());
  for (uint64_t v : values) {
    uint64_t got = 0;
    ASSERT_TRUE(r.varint(got));
    EXPECT_EQ(got, v);
  }
  EXPECT_TRUE(r.at_end());
}

TEST(Wire, VarintEncodingIsLeb128) {
  ByteWriter w;
  w.varint(300);
  EXPECT_EQ(w.data(), (std::vector<uint8_t>{0xac, 0x02}));
}

TEST(Wire, SignedVarintRoundTrip) {
  const std::vector<int64_t> values = {0, -1, 1, -64, 64, -1000000,
                                       std::numeric_limits<int64_t>::min(),
                                       std::numeric_limits<int64_t>::max()};
  ByteWriter w;
  for (int64_t v : values) w.svarint(v);
  ByteReader r(w.data());
  for (int64_t v : values) {
    int64_t got = 0;
    ASSERT_TRUE(r.svarint(got));
    EXPECT_EQ(got, v);
  }
}

TEST(Wire, TruncatedInputFails) {
  const std::vector<uint8_t> bad = {0x80, 0x80};  // varint continues past end
  ByteReader r(bad);
  uint64_t v = 0;
  EXPECT_FALSE(r.varint(v));
  EXPECT_FALSE(r.ok());
  uint8_t b = 0;
  EXPECT_FALSE(r.u8(b));  // stays failed
}

TEST(Wire, OverlongVarintFails) {
  std::vector<uint8_t> bad(11, 0xff);
  ByteReader r(bad);
  uint64_t v = 0;
  EXPECT_FALSE(r.varint(v));
}

TEST(Wire, BlobLengthBeyondBufferFails) {
  ByteWriter w;
  w.varint(1000);
  w.u8(1);
  ByteReader r(w.data());
  std::span<const uint8_t> out;
  EXPECT_FALSE(r.blob(out));
}

TEST(Wire, FixedWidthLittleEndian) {
  ByteWriter w;
  w.u32le(0x11223344);
  w.u64le(0x0102030405060708ull);
  EXPECT_EQ(w.data()[0], 0x44);
  EXPECT_EQ(w.data()[4], 0x08);
  ByteReader r(w.data());
  uint32_t a = 0;
  uint64_t b = 0;
  ASSERT_TRUE(r.u32le(a) && r.u64le(b));
  EXPECT_EQ(a, 0x11223344u);
  EXPECT_EQ(b, 0x0102030405060708ull);
}
