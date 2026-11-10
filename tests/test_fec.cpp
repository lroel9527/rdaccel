// Forward error correction: parity encoding and rebuilding a lost packet.
#include <gtest/gtest.h>

#include <map>
#include <vector>

#include "net/fec.h"

using namespace rdacore::net;

namespace {

std::vector<uint8_t> Body(size_t n, uint8_t seed) {
  std::vector<uint8_t> b(n);
  for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed * 31 + i * 7);
  return b;
}

}  // namespace

TEST(Fec, RebuildsAnySingleLostPacket) {
  // Bodies of different lengths (the last packet of a picture is short).
  const std::map<uint64_t, std::vector<uint8_t>> sent = {
      {10, Body(1176, 1)}, {11, Body(1176, 2)}, {13, Body(300, 3)}, {14, Body(1, 4)}, {20, Body(900, 5)}};
  fec::Parity p;
  for (const auto& [pn, b] : sent) fec::Add(p, pn, b);
  // Through the wire.
  fec::Parity got;
  uint64_t parity_pn = 0;
  ASSERT_TRUE(fec::Decode(fec::Encode(21, p), &parity_pn, &got));
  EXPECT_EQ(parity_pn, 21u);
  EXPECT_EQ(got.pns, (std::vector<uint64_t>{10, 11, 13, 14, 20}));
  for (const auto& [lost, body] : sent) {
    auto known = sent;
    known.erase(lost);
    auto rebuilt = fec::Recover(got, lost, known);
    ASSERT_TRUE(rebuilt) << lost;
    EXPECT_EQ(*rebuilt, body) << lost;
  }
}

TEST(Fec, TwoLostCannotBeRebuilt) {
  fec::Parity p;
  fec::Add(p, 1, Body(100, 1));
  fec::Add(p, 2, Body(100, 2));
  fec::Add(p, 3, Body(100, 3));
  const std::map<uint64_t, std::vector<uint8_t>> known = {{1, Body(100, 1)}};
  EXPECT_FALSE(fec::Recover(p, 2, known));
  EXPECT_FALSE(fec::Recover(p, 7, known));  // not protected by this parity
}

TEST(Fec, RejectsBadParity) {
  uint64_t pn = 0;
  fec::Parity out;
  EXPECT_FALSE(fec::Decode({}, &pn, &out));
  const std::vector<uint8_t> zero_count = {5, 0, 0, 0};
  EXPECT_FALSE(fec::Decode(zero_count, &pn, &out));
  const std::vector<uint8_t> repeat = {5, 2, 7, 0, 0, 0};  // 7 then 7 again
  EXPECT_FALSE(fec::Decode(repeat, &pn, &out));
  const std::vector<uint8_t> too_many = {5, 200, 1};
  EXPECT_FALSE(fec::Decode(too_many, &pn, &out));
}
