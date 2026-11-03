#include <gtest/gtest.h>

#include <vector>

#include "codec/hevc_bitstream.h"

using namespace rdacore::codec;

namespace {

// Minimal fake NAL units: the first byte carries the type (type << 1).
std::vector<uint8_t> Nal(uint8_t type, std::vector<uint8_t> body) {
  body.insert(body.begin(), {static_cast<uint8_t>(type << 1), 0x01});
  return body;
}

}  // namespace

TEST(HevcBitstream, SplitAnnexBHandlesThreeAndFourByteStartCodes) {
  const std::vector<uint8_t> s = {0, 0, 0, 1, 0x40, 0x01, 0xaa, 0, 0, 1, 0x42, 0x01, 0xbb, 0xcc};
  auto nals = SplitAnnexB(s);
  ASSERT_EQ(nals.size(), 2u);
  EXPECT_EQ(std::vector<uint8_t>(nals[0].begin(), nals[0].end()),
            (std::vector<uint8_t>{0x40, 0x01, 0xaa}));
  EXPECT_EQ(std::vector<uint8_t>(nals[1].begin(), nals[1].end()),
            (std::vector<uint8_t>{0x42, 0x01, 0xbb, 0xcc}));
}

TEST(HevcBitstream, AnnexBToAccessUnitSplitsParameterSets) {
  std::vector<uint8_t> annexb;
  AppendAnnexB(Nal(kNalAud, {0x50}), annexb);
  AppendAnnexB(Nal(kNalVps, {1}), annexb);
  AppendAnnexB(Nal(kNalSps, {2}), annexb);
  AppendAnnexB(Nal(kNalPps, {3}), annexb);
  AppendAnnexB(Nal(kNalPrefixSei, {4}), annexb);
  AppendAnnexB(Nal(kNalIdrWRadl, {5, 6, 7}), annexb);

  auto au = AnnexBToAccessUnit(annexb);
  EXPECT_TRUE(au.keyframe);
  ASSERT_EQ(au.parameter_sets.size(), 3u);
  EXPECT_EQ(HevcNalTypeOf(au.parameter_sets[0]), kNalVps);
  EXPECT_EQ(HevcNalTypeOf(au.parameter_sets[2]), kNalPps);

  std::vector<std::span<const uint8_t>> nals;
  ASSERT_TRUE(SplitLengthPrefixed(au.nal_data, nals));
  ASSERT_EQ(nals.size(), 2u);  // SEI + slice, AUD dropped
  EXPECT_EQ(HevcNalTypeOf(nals[0]), kNalPrefixSei);
  EXPECT_EQ(HevcNalTypeOf(nals[1]), kNalIdrWRadl);
}

TEST(HevcBitstream, NonIrapIsNotKeyframe) {
  std::vector<uint8_t> annexb;
  AppendAnnexB(Nal(1 /* TRAIL_R */, {9}), annexb);
  EXPECT_FALSE(AnnexBToAccessUnit(annexb).keyframe);
}

TEST(HevcBitstream, RoundTripThroughWireLayout) {
  std::vector<uint8_t> annexb;
  AppendAnnexB(Nal(kNalVps, {1}), annexb);
  AppendAnnexB(Nal(kNalSps, {2}), annexb);
  AppendAnnexB(Nal(kNalPps, {3}), annexb);
  AppendAnnexB(Nal(kNalCra, {4, 5}), annexb);
  auto au = AnnexBToAccessUnit(annexb);

  std::vector<uint8_t> back;
  ASSERT_TRUE(AccessUnitToAnnexB(au.parameter_sets, au.nal_data, back));
  EXPECT_EQ(back, annexb);
}

TEST(HevcBitstream, MalformedLengthPrefixRejected) {
  const std::vector<uint8_t> bad = {0, 0, 0, 9, 1, 2};
  std::vector<std::span<const uint8_t>> nals;
  EXPECT_FALSE(SplitLengthPrefixed(bad, nals));
  std::vector<uint8_t> out;
  EXPECT_FALSE(AccessUnitToAnnexB({}, bad, out));
}
