#include <gtest/gtest.h>

#include <tuple>
#include <vector>

#include "proto/messages.h"

using namespace rdacore;
using namespace rdacore::proto;

namespace {

std::vector<Message> SampleMessages() {
  VideoData video;
  video.frame_id = 42;
  video.keyframe = true;
  video.parameter_sets = {{0x40, 0x01, 0x0c}, {0x42, 0x01}, {0x44, 0x01, 0xc1}};
  video.regions = {{0, 0, 64, 64}, {128, 64, 64, 32}};
  video.nal_data = {0, 0, 0, 3, 0x26, 0x01, 0xaf};

  CursorShape cursor;
  cursor.shape_id = 7;
  cursor.hotspot_x = 1;
  cursor.hotspot_y = 2;
  cursor.width = 2;
  cursor.height = 1;
  cursor.bgra = {1, 2, 3, 4, 5, 6, 7, 8};

  VideoData area = video;
  area.keyframe = false;
  area.parameter_sets.clear();
  area.regions.clear();
  area.stream = 3;
  area.x = 640;
  area.y = -2;

  return {
      Hello{kProtocolVersion, 3840, 2160, VideoCodec::kHevc},
      area,
      FrameBegin{42, 123456789},
      video,
      FrameEnd{42},
      CopyRect{43, {0, 64, 1920, 1000}, 0, -64},
      CursorPos{-5, 1080, false, 9, 123456},
      cursor,
      InputEvent{InputKind::kScroll, 100, 200, 0, -3, 0, 0x100000},
      InputEvent{InputKind::kText, 0, 0, 0, 0, 0x1F600, 0},
      Feedback{42, 1000, 2000},
      LosslessData{8, true, {1, 2, 3}},
      KeyframeRequest{77},
  };
}

}  // namespace

TEST(Messages, RoundTripEachType) {
  for (const Message& m : SampleMessages()) {
    StreamParser p;
    p.Append(Encode(m));
    auto got = p.Next();
    ASSERT_TRUE(got.has_value()) << "type index " << m.index() << ": " << p.error();
    EXPECT_EQ(*got, m);
    EXPECT_FALSE(p.Next().has_value());
    EXPECT_TRUE(p.error().empty());
  }
}

TEST(Messages, ParserHandlesByteByByteDelivery) {
  std::vector<uint8_t> stream;
  const auto msgs = SampleMessages();
  for (const auto& m : msgs) EncodeTo(m, stream);

  StreamParser p;
  std::vector<Message> out;
  for (uint8_t b : stream) {
    p.Append({&b, 1});
    while (auto m = p.Next()) out.push_back(std::move(*m));
  }
  ASSERT_TRUE(p.error().empty()) << p.error();
  EXPECT_EQ(out, msgs);
}

TEST(Messages, UnknownTypesAreSkipped) {
  // A later protocol version may add message types; this one skips them.
  std::vector<uint8_t> bytes = {0xee, 0x02, 0xaa, 0xbb};
  const auto end = Encode(FrameEnd{7});
  bytes.insert(bytes.end(), end.begin(), end.end());
  StreamParser p;
  p.Append(bytes);
  auto m = p.Next();
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(std::get<FrameEnd>(*m).frame_id, 7u);
  EXPECT_TRUE(p.error().empty());
  EXPECT_EQ(p.skipped(), 1u);
}

TEST(Messages, InputKindsFromALaterVersionAreSkipped) {
  // An InputEvent of a kind this version does not know: skipped, the stream
  // goes on (a 0.3 host disconnected here, hence the text-input capability).
  InputEvent future{InputKind::kKeyDown, 1, 2, 0, 0, 4, 0};
  auto bytes = Encode(future);
  bytes[2] = 42;  // the kind byte, after the type and length
  const auto end = Encode(FrameEnd{7});
  bytes.insert(bytes.end(), end.begin(), end.end());
  StreamParser p;
  p.Append(bytes);
  auto m = p.Next();
  ASSERT_TRUE(m.has_value()) << p.error();
  EXPECT_EQ(std::get<FrameEnd>(*m).frame_id, 7u);
  EXPECT_EQ(p.skipped(), 1u);
}

TEST(Messages, TextMustBeAUnicodeScalar) {
  for (uint32_t bad : {0u, 0xD800u, 0xDFFFu, 0x110000u}) {
    StreamParser p;
    p.Append(Encode(InputEvent{InputKind::kText, 0, 0, 0, 0, bad, 0}));
    EXPECT_FALSE(p.Next().has_value()) << bad;
    EXPECT_FALSE(p.error().empty()) << bad;
  }
}

TEST(Messages, TrailingFieldsAreIgnored) {
  // ... and may append fields to existing ones.
  auto bytes = Encode(FrameEnd{1});
  bytes[1] += 2;
  bytes.push_back(0x05);
  bytes.push_back(0x06);
  StreamParser p;
  p.Append(bytes);
  auto m = p.Next();
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(std::get<FrameEnd>(*m).frame_id, 1u);
}

TEST(Messages, HandshakeAndSessionMessagesRoundTrip) {
  ViewerHello vh;
  vh.auth_token = {1, 2, 3};
  AppMessage am{0x10001, std::vector<uint8_t>(100000, 7)};
  const std::vector<Message> in = {vh, Ping{123456}, Goodbye{GoodbyeReason::kIncompatible}, am, Welcome{},
                                   Hello{kProtocolVersion, 640, 360, VideoCodec::kHevc, 4, kCapsAll}};
  std::vector<uint8_t> bytes;
  for (const auto& m : in) EncodeTo(m, bytes);
  StreamParser p;
  p.Append(bytes);
  for (const auto& want : in) {
    auto got = p.Next();
    ASSERT_TRUE(got.has_value()) << p.error();
    EXPECT_EQ(*got, want);
  }
}

TEST(Messages, Version3HelloStillParses) {
  // version 3, 640x360, HEVC; no min_version / caps.
  const std::vector<uint8_t> v3 = {static_cast<uint8_t>(MsgType::kHello), 6, 3, 0x80, 0x05, 0xe8, 0x02, 2};
  StreamParser p;
  p.Append(v3);
  auto m = p.Next();
  ASSERT_TRUE(m.has_value()) << p.error();
  const auto& h = std::get<Hello>(*m);
  EXPECT_EQ(h.version, 3u);
  EXPECT_EQ(h.width, 640u);
  EXPECT_EQ(h.min_version, 3u);
  EXPECT_EQ(h.caps, 0u);
}

TEST(Messages, CursorShapeSizeMustMatch) {
  CursorShape c;
  c.width = 2;
  c.height = 2;
  c.bgra = {1, 2, 3, 4};  // should be 16 bytes
  StreamParser p;
  p.Append(Encode(c));
  EXPECT_FALSE(p.Next().has_value());
  EXPECT_FALSE(p.error().empty());
}

TEST(Messages, H264IsRejected) {
  // Byte 1 was H.264 in early drafts; the protocol is HEVC-only now.
  auto bytes = Encode(Hello{kProtocolVersion, 1, 1, VideoCodec::kHevc});
  bytes[5] = 1;  // type, length, version, width, height, then the codec
  StreamParser p;
  p.Append(bytes);
  EXPECT_FALSE(p.Next().has_value());
}

TEST(Messages, HugeCountIsRejectedWithoutAllocating) {
  // VideoData claiming 2^31 parameter sets in a 10-byte payload.
  std::vector<uint8_t> bytes = {static_cast<uint8_t>(MsgType::kVideoData), 8, 1, 0,
                                0x80, 0x80, 0x80, 0x80, 0x08, 0};
  StreamParser p;
  p.Append(bytes);
  EXPECT_FALSE(p.Next().has_value());
  EXPECT_FALSE(p.error().empty());
}

TEST(Messages, FullScreenVideoDataKeepsTheVersion2Layout) {
  // Recordings made before video areas existed must still parse, so a
  // stream-0 message carries no trailing area fields.
  VideoData v;
  v.frame_id = 1;
  v.nal_data = {0, 0, 0, 1, 0x26};
  VideoData a = v;
  a.stream = 1;
  EXPECT_EQ(Encode(a).size(), Encode(v).size() + 3);
  // A zero stream id written explicitly is malformed (it would be ambiguous).
  auto bytes = Encode(v);
  bytes[1] += 3;  // payload length (one-byte varint here)
  bytes.insert(bytes.end(), {0, 0, 0});
  StreamParser p;
  p.Append(bytes);
  EXPECT_FALSE(p.Next().has_value());
  EXPECT_FALSE(p.error().empty());
}

TEST(Messages, HelloScreenSizeIsBounded) {
  // A hostile host must not make the viewer allocate a gigantic frame
  // (found by fuzz_viewer). 0 and anything above kMaxScreenSide is rejected.
  for (auto [w, h, ok] : {std::tuple{1u, 1u, true}, std::tuple{kMaxScreenSide, kMaxScreenSide, true},
                          std::tuple{0u, 360u, false}, std::tuple{640u, 0u, false},
                          std::tuple{kMaxScreenSide + 1, 360u, false}, std::tuple{640u, 1u << 30, false}}) {
    StreamParser p;
    p.Append(Encode(Hello{kProtocolVersion, w, h, VideoCodec::kHevc}));
    EXPECT_EQ(p.Next().has_value(), ok) << w << "x" << h;
  }
}
