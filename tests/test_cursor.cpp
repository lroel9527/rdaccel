#include <gtest/gtest.h>

#include <vector>

#include "proto/messages.h"
#include "rdaccel/cursor.h"
#include "session/cursor_channel.h"

using namespace rdaccel;

namespace {

// Records what the sender puts on the wire.
class Wire final : public transport::ITransport {
 public:
  bool Send(transport::Channel ch, std::span<const uint8_t> bytes) override {
    EXPECT_EQ(ch, transport::Channel::kCursor);
    parser.Append(bytes);
    while (auto m = parser.Next()) msgs.push_back(std::move(*m));
    return true;
  }
  void SetReceiver(ReceiveFn) override {}
  void Close() override {}

  int Count(size_t type_index) const {
    int n = 0;
    for (const auto& m : msgs) n += m.index() == type_index;
    return n;
  }
  proto::StreamParser parser;
  std::vector<proto::Message> msgs;
};

constexpr size_t kPos = 5, kShape = 6;  // indices in proto::Message

CursorImage Arrow(uint8_t shade, int32_t size = 32) {
  CursorImage c;
  c.width = size;
  c.height = size;
  c.hotspot_x = 1;
  c.hotspot_y = 2;
  c.bgra.assign(static_cast<size_t>(size) * size * 4, 0);
  for (int32_t y = 0; y < size; ++y) {
    for (int32_t x = 0; x <= y / 2; ++x) {
      uint8_t* p = &c.bgra[(static_cast<size_t>(y) * size + x) * 4];
      p[0] = p[1] = p[2] = shade;
      p[3] = 255;
    }
  }
  return c;
}

}  // namespace

TEST(Cursor, MessagesAreSmallAndRoundTrip) {
  ASSERT_EQ(std::variant_size_v<proto::Message>, 10u);
  ASSERT_TRUE((std::holds_alternative<proto::CursorPos>(proto::Message{proto::CursorPos{}})));
  ASSERT_EQ(proto::Message{proto::CursorPos{}}.index(), kPos);
  ASSERT_EQ(proto::Message{proto::CursorShape{}}.index(), kShape);

  const auto img = Arrow(200, 64);
  proto::CursorShape s{9, img.hotspot_x, img.hotspot_y, 64, 64, img.bgra};
  const auto bytes = proto::Encode(s);
  // 16 kB of mostly transparent pixels deflate to a few hundred bytes.
  EXPECT_LT(bytes.size(), 600u);
  proto::StreamParser p;
  p.Append(bytes);
  auto got = p.Next();
  ASSERT_TRUE(got);
  EXPECT_EQ(std::get<proto::CursorShape>(*got), s);

  // A position costs a handful of bytes.
  EXPECT_LT(proto::Encode(proto::CursorPos{1919, 1079, true, s.shape_id, 1'234'567'890}).size(), 20u);
}

TEST(Cursor, OversizedShapeIsRejected) {
  proto::CursorShape s;
  s.width = proto::kMaxCursorSize + 1;
  s.height = 1;
  s.bgra.assign(s.width * 4, 0);
  proto::StreamParser p;
  p.Append(proto::Encode(s));
  EXPECT_FALSE(p.Next());
  EXPECT_FALSE(p.error().empty());
}

TEST(Cursor, EachImageIsSentOnceAndPositionsOnlyWhenTheyChange) {
  Wire wire;
  session::CursorSender tx(&wire);
  const auto arrow = Arrow(0), beam = Arrow(255);
  tx.SetImage(arrow);
  tx.SetPosition(10, 20, true, 1);
  tx.SetPosition(10, 20, true, 2);  // unchanged: nothing
  tx.SetImage(arrow);               // unchanged: nothing
  EXPECT_EQ(wire.Count(kShape), 1);
  EXPECT_EQ(wire.Count(kPos), 1);

  tx.SetImage(beam);   // new image + a position that refers to it
  tx.SetImage(arrow);  // known: only a position
  tx.SetImage(beam);
  EXPECT_EQ(wire.Count(kShape), 2);
  EXPECT_EQ(wire.Count(kPos), 4);
  tx.SetPosition(11, 20, true, 3);
  tx.SetPosition(11, 20, false, 4);
  EXPECT_EQ(wire.Count(kPos), 6);
  const auto st = tx.stats();
  EXPECT_EQ(st.images, 2u);
  EXPECT_EQ(st.positions, 6u);

  // The viewer side resolves every position to the right image.
  session::CursorReceiver rx;
  std::vector<CursorState> states;
  std::vector<CursorImage> images;
  for (auto& m : wire.msgs) {
    if (auto* s = std::get_if<proto::CursorShape>(&m)) rx.OnShape(std::move(*s));
    if (auto* p = std::get_if<proto::CursorPos>(&m)) {
      CursorState cs;
      rx.OnPos(*p, cs);
      ASSERT_TRUE(cs.image);
      images.push_back(*cs.image);
      states.push_back(cs);
    }
  }
  ASSERT_EQ(states.size(), 6u);
  EXPECT_EQ(images[0], arrow);
  EXPECT_TRUE(states[0].image_changed);
  EXPECT_EQ(images[1], beam);
  EXPECT_TRUE(states[1].image_changed);
  EXPECT_EQ(images[2], arrow);
  EXPECT_EQ(images[3], beam);
  EXPECT_FALSE(states[4].image_changed);
  EXPECT_EQ(states[4].x, 11);
  EXPECT_TRUE(states[4].visible);
  EXPECT_FALSE(states[5].visible);
  EXPECT_EQ(states[5].time_us, 4u);
}

TEST(Cursor, BothEndsForgetTheSameImages) {
  Wire wire;
  session::CursorSender tx(&wire);
  tx.SetPosition(0, 0, true, 0);
  // One more image than the cache holds, then the first one again: it was
  // evicted on both ends, so it must be sent again.
  for (size_t i = 0; i <= session::kCursorCacheSize; ++i) tx.SetImage(Arrow(static_cast<uint8_t>(i)));
  tx.SetImage(Arrow(0));
  tx.SetImage(Arrow(2));  // still cached
  EXPECT_EQ(wire.Count(kShape), static_cast<int>(session::kCursorCacheSize) + 2);

  session::CursorReceiver rx;
  for (auto& m : wire.msgs) {
    if (auto* s = std::get_if<proto::CursorShape>(&m)) rx.OnShape(std::move(*s));
    if (auto* p = std::get_if<proto::CursorPos>(&m)) {
      CursorState cs;
      rx.OnPos(*p, cs);
      EXPECT_TRUE(cs.image || cs.image_id == 0) << "position refers to an image the viewer no longer has";
    }
  }
}

TEST(Cursor, InvalidImagesAreIgnored) {
  Wire wire;
  session::CursorSender tx(&wire);
  CursorImage bad = Arrow(0);
  bad.bgra.pop_back();
  tx.SetImage(bad);
  tx.SetImage(Arrow(0, 300));  // too large
  tx.SetImage(CursorImage{});
  EXPECT_EQ(wire.Count(kShape), 0);
}

TEST(Cursor, DrawBlendsAndClips) {
  constexpr int W = 8, H = 4;
  std::vector<uint8_t> px(W * H * 4, 100);
  CursorImage c;
  c.width = 2;
  c.height = 2;
  c.hotspot_x = 1;
  c.hotspot_y = 1;
  // opaque white, half-transparent black (premultiplied), transparent, opaque red
  c.bgra = {255, 255, 255, 255, 0, 0, 0, 128, 0, 0, 0, 0, 0, 0, 255, 255};
  DrawCursor(c, 1, 1, px.data(), W, H, W * 4);
  EXPECT_EQ(px[0], 255);                 // (0,0) white
  EXPECT_EQ(px[4], 50);                  // (1,0) 100 * 127/255
  EXPECT_EQ(px[W * 4], 100);             // (0,1) untouched
  EXPECT_EQ(px[W * 4 + 4 + 2], 255);     // (1,1) red
  EXPECT_EQ(px[W * 4 + 4 + 0], 0);
  // Partly off-screen: no crash, only the visible pixel is drawn.
  DrawCursor(c, 0, 0, px.data(), W, H, W * 4);
  DrawCursor(c, W, H, px.data(), W, H, W * 4);
  EXPECT_EQ(px[(H - 1) * W * 4 + (W - 1) * 4], 255);
}
