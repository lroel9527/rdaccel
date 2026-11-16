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

// H.264: the type is the low five bits of the first byte (here with
// nal_ref_idc 3, as encoders write parameter sets and IDR slices).
namespace {
std::vector<uint8_t> H264Nal(uint8_t type, std::vector<uint8_t> body) {
  body.insert(body.begin(), static_cast<uint8_t>(0x60 | type));
  return body;
}
}  // namespace

TEST(H264Bitstream, NalTypesAndQuestions) {
  EXPECT_EQ(H264NalTypeOf(H264Nal(kH264NalSps, {1})), kH264NalSps);
  EXPECT_EQ(H264NalTypeOf(std::vector<uint8_t>{0x65, 0x88}), kH264NalIdr);
  EXPECT_EQ(H264NalTypeOf(std::vector<uint8_t>{0x41, 0x9a}), kH264NalSlice);
  EXPECT_EQ(H264NalTypeOf({}), 0xff);
  EXPECT_TRUE(IsParameterSetNal(VideoCodec::kH264, kH264NalSps));
  EXPECT_TRUE(IsParameterSetNal(VideoCodec::kH264, kH264NalPps));
  EXPECT_FALSE(IsParameterSetNal(VideoCodec::kH264, kH264NalIdr));
  EXPECT_TRUE(IsKeyframeNal(VideoCodec::kH264, kH264NalIdr));
  EXPECT_FALSE(IsKeyframeNal(VideoCodec::kH264, kH264NalSlice));
  EXPECT_TRUE(IsAudNal(VideoCodec::kH264, kH264NalAud));
  EXPECT_FALSE(IsAudNal(VideoCodec::kH264, kNalAud));
  // The HEVC answers are unchanged.
  EXPECT_TRUE(IsKeyframeNal(VideoCodec::kHevc, kNalIdrWRadl));
  EXPECT_TRUE(IsParameterSetNal(VideoCodec::kHevc, kNalVps));
  EXPECT_EQ(NalTypeOf(VideoCodec::kHevc, Nal(kNalSps, {})), kNalSps);
  EXPECT_EQ(NalTypeOf(VideoCodec::kH264, H264Nal(kH264NalPps, {})), kH264NalPps);
}

TEST(H264Bitstream, AnnexBToAccessUnitSplitsParameterSets) {
  std::vector<uint8_t> annexb;
  AppendAnnexB(H264Nal(kH264NalAud, {0xf0}), annexb);
  AppendAnnexB(H264Nal(kH264NalSps, {0x64, 0x00, 0x28}), annexb);
  AppendAnnexB(H264Nal(kH264NalPps, {0xee}), annexb);
  AppendAnnexB(H264Nal(kH264NalSei, {4}), annexb);
  AppendAnnexB(H264Nal(kH264NalIdr, {5, 6, 7}), annexb);

  auto au = AnnexBToAccessUnit(annexb, VideoCodec::kH264);
  EXPECT_TRUE(au.keyframe);
  ASSERT_EQ(au.parameter_sets.size(), 2u);
  EXPECT_EQ(H264NalTypeOf(au.parameter_sets[0]), kH264NalSps);
  EXPECT_EQ(H264NalTypeOf(au.parameter_sets[1]), kH264NalPps);
  std::vector<std::span<const uint8_t>> nals;
  ASSERT_TRUE(SplitLengthPrefixed(au.nal_data, nals));
  ASSERT_EQ(nals.size(), 2u);  // AUD dropped
  EXPECT_EQ(H264NalTypeOf(nals[0]), kH264NalSei);
  EXPECT_EQ(H264NalTypeOf(nals[1]), kH264NalIdr);

  // A P picture is no keyframe; back to Annex B unchanged.
  std::vector<uint8_t> p;
  AppendAnnexB(H264Nal(kH264NalSlice, {9}), p);
  auto pau = AnnexBToAccessUnit(p, VideoCodec::kH264);
  EXPECT_FALSE(pau.keyframe);
  EXPECT_TRUE(pau.parameter_sets.empty());
  std::vector<uint8_t> back;
  ASSERT_TRUE(AccessUnitToAnnexB(au.parameter_sets, au.nal_data, back));
  std::vector<uint8_t> expect;
  for (const auto& n : {H264Nal(kH264NalSps, {0x64, 0x00, 0x28}), H264Nal(kH264NalPps, {0xee}),
                        H264Nal(kH264NalSei, {4}), H264Nal(kH264NalIdr, {5, 6, 7})}) {
    AppendAnnexB(n, expect);
  }
  EXPECT_EQ(back, expect);
}

TEST(H264Bitstream, HevcParsingOfAnH264StreamWouldBeWrong) {
  // Why the codec matters: read as HEVC, an H.264 IDR (0x65) is type 50 and
  // the SPS is no parameter set, so the keyframe would go unnoticed.
  std::vector<uint8_t> annexb;
  AppendAnnexB(H264Nal(kH264NalSps, {1}), annexb);
  AppendAnnexB(H264Nal(kH264NalIdr, {2}), annexb);
  auto au = AnnexBToAccessUnit(annexb);  // HEVC by default
  EXPECT_FALSE(au.keyframe);
  EXPECT_TRUE(au.parameter_sets.empty());
}

TEST(VideoCodecDetection, TellsTheCodecFromTheParameterSets) {
  EXPECT_EQ(DetectCodec({Nal(kNalVps, {1}), Nal(kNalSps, {2}), Nal(kNalPps, {3})}), VideoCodec::kHevc);
  EXPECT_EQ(DetectCodec({H264Nal(kH264NalSps, {0x64}), H264Nal(kH264NalPps, {1})}), VideoCodec::kH264);
  // nal_ref_idc 1: 0x27, which as HEVC would be an IDR slice; still H.264
  // without a VPS.
  EXPECT_EQ(DetectCodec({{0x27, 0x42}, {0x28, 0xce}}), VideoCodec::kH264);
  EXPECT_EQ(DetectCodec({}), std::nullopt);
  EXPECT_EQ(DetectCodec({{}, {0xff}}), std::nullopt);
  EXPECT_STREQ(VideoCodecName(VideoCodec::kH264), "H.264");
  EXPECT_STREQ(VideoCodecName(VideoCodec::kHevc), "HEVC");
}
