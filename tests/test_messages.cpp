#include <gtest/gtest.h>

#include <vector>

#include "proto/messages.h"

using namespace rdaccel;
using namespace rdaccel::proto;

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

  return {
      Hello{kProtocolVersion, 3840, 2160, VideoCodec::kHevc},
      FrameBegin{42, 123456789},
      video,
      FrameEnd{42},
      CopyRect{43, {0, 64, 1920, 1000}, 0, -64},
      CursorPos{-5, 1080, false},
      cursor,
      InputEvent{InputKind::kScroll, 100, 200, 0, -3, 0, 0x100000},
      Feedback{42, 1000, 2000},
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

TEST(Messages, UnknownTypeIsAnError) {
  StreamParser p;
  const std::vector<uint8_t> bad = {0xee, 0x00};
  p.Append(bad);
  EXPECT_FALSE(p.Next().has_value());
  EXPECT_FALSE(p.error().empty());
}

TEST(Messages, TrailingBytesInPayloadAreAnError) {
  auto bytes = Encode(FrameEnd{1});
  // Grow the declared payload by one byte and append garbage.
  bytes[1] += 1;
  bytes.push_back(0x00);
  StreamParser p;
  p.Append(bytes);
  EXPECT_FALSE(p.Next().has_value());
  EXPECT_FALSE(p.error().empty());
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
  bytes.back() = 1;
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
