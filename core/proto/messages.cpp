#include "proto/messages.h"

#include <type_traits>

#include <zlib.h>

#include "proto/wire.h"

namespace rdacore::proto {
namespace {

void PutRect(ByteWriter& w, const Rect& r) {
  w.svarint(r.x);
  w.svarint(r.y);
  w.svarint(r.w);
  w.svarint(r.h);
}

bool GetRect(ByteReader& r, Rect& out) {
  return r.svarint32(out.x) && r.svarint32(out.y) && r.svarint32(out.w) && r.svarint32(out.h);
}

bool GetBool(ByteReader& r, bool& out) {
  uint8_t b = 0;
  if (!r.u8(b) || b > 1) return false;
  out = b == 1;
  return true;
}

// A count read from the wire must not claim more elements than there are
// bytes left, otherwise a tiny malicious message could make us allocate a lot.
bool GetCount(ByteReader& r, uint32_t& n) {
  return r.varint32(n) && n <= r.remaining();
}

std::vector<uint8_t> ToVec(std::span<const uint8_t> s) { return {s.begin(), s.end()}; }

// --- per-message payload encoders -----------------------------------------

MsgType Write(ByteWriter& w, const Hello& m) {
  w.varint(m.version);
  w.varint(m.width);
  w.varint(m.height);
  w.u8(static_cast<uint8_t>(m.codec));
  w.varint(m.min_version);
  w.varint(m.caps);
  return MsgType::kHello;
}

MsgType Write(ByteWriter& w, const ViewerHello& m) {
  w.varint(m.version);
  w.varint(m.min_version);
  w.varint(m.caps);
  w.blob(m.auth_token);
  return MsgType::kViewerHello;
}

MsgType Write(ByteWriter& w, const Welcome& m) {
  w.varint(m.version);
  w.varint(m.min_version);
  w.varint(m.caps);
  return MsgType::kWelcome;
}

MsgType Write(ByteWriter& w, const Ping& m) {
  w.varint(m.time_us);
  return MsgType::kPing;
}

MsgType Write(ByteWriter& w, const Goodbye& m) {
  w.u8(static_cast<uint8_t>(m.reason));
  return MsgType::kGoodbye;
}

MsgType Write(ByteWriter& w, const AppMessage& m) {
  w.varint(m.type);
  w.blob(m.payload);
  return MsgType::kAppMessage;
}

MsgType Write(ByteWriter& w, const FrameBegin& m) {
  w.varint(m.frame_id);
  w.varint(m.capture_time_us);
  return MsgType::kFrameBegin;
}

MsgType Write(ByteWriter& w, const VideoData& m) {
  w.varint(m.frame_id);
  w.u8(m.keyframe ? 1 : 0);
  w.varint(m.parameter_sets.size());
  for (const auto& ps : m.parameter_sets) w.blob(ps);
  w.varint(m.regions.size());
  for (const auto& r : m.regions) PutRect(w, r);
  w.blob(m.nal_data);
  if (m.stream != 0) {
    w.u8(m.stream);
    w.svarint(m.x);
    w.svarint(m.y);
  }
  return MsgType::kVideoData;
}

MsgType Write(ByteWriter& w, const FrameEnd& m) {
  w.varint(m.frame_id);
  return MsgType::kFrameEnd;
}

MsgType Write(ByteWriter& w, const CopyRect& m) {
  w.varint(m.frame_id);
  PutRect(w, m.src);
  w.svarint(m.dx);
  w.svarint(m.dy);
  return MsgType::kCopyRect;
}

MsgType Write(ByteWriter& w, const CursorPos& m) {
  w.svarint(m.x);
  w.svarint(m.y);
  w.u8(m.visible ? 1 : 0);
  w.varint(m.shape_id);
  w.varint(m.time_us);
  return MsgType::kCursorPos;
}

MsgType Write(ByteWriter& w, const CursorShape& m) {
  w.varint(m.shape_id);
  w.svarint(m.hotspot_x);
  w.svarint(m.hotspot_y);
  w.varint(m.width);
  w.varint(m.height);
  // Cursors are mostly transparent or flat: deflate shrinks them 5-20x.
  uLongf n = compressBound(static_cast<uLong>(m.bgra.size()));
  std::vector<uint8_t> z(n);
  if (compress2(z.data(), &n, m.bgra.data(), static_cast<uLong>(m.bgra.size()), 6) != Z_OK) n = 0;
  z.resize(n);
  w.blob(z);
  return MsgType::kCursorShape;
}

MsgType Write(ByteWriter& w, const InputEvent& m) {
  w.u8(static_cast<uint8_t>(m.kind));
  w.svarint(m.x);
  w.svarint(m.y);
  w.svarint(m.delta_x);
  w.svarint(m.delta_y);
  w.varint(m.code);
  w.varint(m.modifiers);
  return MsgType::kInputEvent;
}

MsgType Write(ByteWriter& w, const KeyframeRequest& m) {
  w.varint(m.last_frame_id);
  return MsgType::kKeyframeRequest;
}

MsgType Write(ByteWriter& w, const Feedback& m) {
  w.varint(m.frame_id);
  w.varint(m.receive_time_us);
  w.varint(m.present_time_us);
  return MsgType::kFeedback;
}

MsgType Write(ByteWriter& w, const LosslessData& m) {
  w.varint(m.frame_id);
  w.u8(m.reset ? 1 : 0);
  w.blob(m.data);
  return MsgType::kLosslessData;
}

// --- per-message payload decoders -----------------------------------------

std::optional<Message> ReadHello(ByteReader& r) {
  Hello m;
  uint8_t codec = 0;
  if (!(r.varint32(m.version) && r.varint32(m.width) && r.varint32(m.height) && r.u8(codec))) {
    return std::nullopt;
  }
  if (codec != static_cast<uint8_t>(VideoCodec::kHevc)) return std::nullopt;
  // A screen size a viewer can allocate (also bounds what a hostile host
  // can make it do).
  if (m.width == 0 || m.height == 0 || m.width > kMaxScreenSide || m.height > kMaxScreenSide) {
    return std::nullopt;
  }
  m.codec = static_cast<VideoCodec>(codec);
  // Version 4 fields; older hosts serve only their own version.
  m.min_version = m.version;
  m.caps = 0;
  if (!r.at_end() && !(r.varint32(m.min_version) && r.varint(m.caps))) return std::nullopt;
  return m;
}

std::optional<Message> ReadViewerHello(ByteReader& r) {
  ViewerHello m;
  std::span<const uint8_t> token;
  if (!(r.varint32(m.version) && r.varint32(m.min_version) && r.varint(m.caps) && r.blob(token))) {
    return std::nullopt;
  }
  if (token.size() > 4096) return std::nullopt;
  m.auth_token = ToVec(token);
  return m;
}

std::optional<Message> ReadWelcome(ByteReader& r) {
  Welcome m;
  if (!(r.varint32(m.version) && r.varint32(m.min_version) && r.varint(m.caps))) return std::nullopt;
  return m;
}

std::optional<Message> ReadPing(ByteReader& r) {
  Ping m;
  if (!r.varint(m.time_us)) return std::nullopt;
  return m;
}

std::optional<Message> ReadGoodbye(ByteReader& r) {
  Goodbye m;
  uint8_t reason = 0;
  if (!r.u8(reason)) return std::nullopt;
  m.reason = reason <= static_cast<uint8_t>(GoodbyeReason::kRejected) ? static_cast<GoodbyeReason>(reason)
                                                                       : GoodbyeReason::kClosed;
  return m;
}

std::optional<Message> ReadAppMessage(ByteReader& r) {
  AppMessage m;
  std::span<const uint8_t> payload;
  if (!(r.varint32(m.type) && r.blob(payload)) || payload.size() > kMaxAppMessage) return std::nullopt;
  m.payload = ToVec(payload);
  return m;
}

std::optional<Message> ReadFrameBegin(ByteReader& r) {
  FrameBegin m;
  if (!(r.varint(m.frame_id) && r.varint(m.capture_time_us))) return std::nullopt;
  return m;
}

std::optional<Message> ReadVideoData(ByteReader& r) {
  VideoData m;
  uint32_t n = 0;
  if (!(r.varint(m.frame_id) && GetBool(r, m.keyframe) && GetCount(r, n))) return std::nullopt;
  m.parameter_sets.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    std::span<const uint8_t> ps;
    if (!r.blob(ps)) return std::nullopt;
    m.parameter_sets.push_back(ToVec(ps));
  }
  if (!GetCount(r, n)) return std::nullopt;
  m.regions.resize(n);
  for (auto& rect : m.regions) {
    if (!GetRect(r, rect)) return std::nullopt;
  }
  std::span<const uint8_t> nal;
  if (!r.blob(nal)) return std::nullopt;
  m.nal_data = ToVec(nal);
  if (!r.at_end()) {
    if (!(r.u8(m.stream) && m.stream != 0 && r.svarint32(m.x) && r.svarint32(m.y))) {
      return std::nullopt;
    }
  }
  return m;
}

std::optional<Message> ReadFrameEnd(ByteReader& r) {
  FrameEnd m;
  if (!r.varint(m.frame_id)) return std::nullopt;
  return m;
}

std::optional<Message> ReadCopyRect(ByteReader& r) {
  CopyRect m;
  if (!(r.varint(m.frame_id) && GetRect(r, m.src) && r.svarint32(m.dx) && r.svarint32(m.dy))) {
    return std::nullopt;
  }
  return m;
}

std::optional<Message> ReadCursorPos(ByteReader& r) {
  CursorPos m;
  if (!(r.svarint32(m.x) && r.svarint32(m.y) && GetBool(r, m.visible) && r.varint(m.shape_id) &&
        r.varint(m.time_us))) {
    return std::nullopt;
  }
  return m;
}

std::optional<Message> ReadCursorShape(ByteReader& r) {
  CursorShape m;
  std::span<const uint8_t> px;
  if (!(r.varint(m.shape_id) && r.svarint32(m.hotspot_x) && r.svarint32(m.hotspot_y) &&
        r.varint32(m.width) && r.varint32(m.height) && r.blob(px))) {
    return std::nullopt;
  }
  if (m.width == 0 || m.height == 0 || m.width > kMaxCursorSize || m.height > kMaxCursorSize) {
    return std::nullopt;
  }
  m.bgra.resize(static_cast<size_t>(m.width) * m.height * 4);
  uLongf n = static_cast<uLongf>(m.bgra.size());
  if (uncompress(m.bgra.data(), &n, px.data(), static_cast<uLong>(px.size())) != Z_OK ||
      n != m.bgra.size()) {
    return std::nullopt;
  }
  return m;
}

std::optional<Message> ReadInputEvent(ByteReader& r, bool& unknown_kind) {
  InputEvent m;
  uint8_t kind = 0;
  if (!(r.u8(kind) && r.svarint32(m.x) && r.svarint32(m.y) && r.svarint32(m.delta_x) &&
        r.svarint32(m.delta_y) && r.varint32(m.code) && r.varint32(m.modifiers))) {
    return std::nullopt;
  }
  if (kind < static_cast<uint8_t>(InputKind::kMouseMove) ||
      kind > static_cast<uint8_t>(InputKind::kText)) {
    // A kind from a later version: the caller skips the message.
    unknown_kind = true;
    return std::nullopt;
  }
  m.kind = static_cast<InputKind>(kind);
  if (m.kind == InputKind::kText && !IsUnicodeScalar(m.code)) return std::nullopt;
  return m;
}

std::optional<Message> ReadKeyframeRequest(ByteReader& r) {
  KeyframeRequest m;
  if (!r.varint(m.last_frame_id)) return std::nullopt;
  return m;
}

std::optional<Message> ReadFeedback(ByteReader& r) {
  Feedback m;
  if (!(r.varint(m.frame_id) && r.varint(m.receive_time_us) && r.varint(m.present_time_us))) {
    return std::nullopt;
  }
  return m;
}

std::optional<Message> ReadLosslessData(ByteReader& r) {
  LosslessData m;
  std::span<const uint8_t> data;
  if (!(r.varint(m.frame_id) && GetBool(r, m.reset) && r.blob(data))) return std::nullopt;
  m.data = ToVec(data);
  return m;
}

// `known` = false for a message type this version does not know (skipped).
std::optional<Message> DecodePayload(MsgType type, std::span<const uint8_t> payload, bool& known) {
  ByteReader r(payload);
  std::optional<Message> m;
  known = true;
  switch (type) {
    case MsgType::kHello: m = ReadHello(r); break;
    case MsgType::kFrameBegin: m = ReadFrameBegin(r); break;
    case MsgType::kVideoData: m = ReadVideoData(r); break;
    case MsgType::kFrameEnd: m = ReadFrameEnd(r); break;
    case MsgType::kCopyRect: m = ReadCopyRect(r); break;
    case MsgType::kCursorPos: m = ReadCursorPos(r); break;
    case MsgType::kCursorShape: m = ReadCursorShape(r); break;
    case MsgType::kInputEvent: {
      bool unknown_kind = false;
      m = ReadInputEvent(r, unknown_kind);
      if (unknown_kind) {
        known = false;
        return std::nullopt;
      }
      break;
    }
    case MsgType::kFeedback: m = ReadFeedback(r); break;
    case MsgType::kLosslessData: m = ReadLosslessData(r); break;
    case MsgType::kKeyframeRequest: m = ReadKeyframeRequest(r); break;
    case MsgType::kViewerHello: m = ReadViewerHello(r); break;
    case MsgType::kPing: m = ReadPing(r); break;
    case MsgType::kGoodbye: m = ReadGoodbye(r); break;
    case MsgType::kAppMessage: m = ReadAppMessage(r); break;
    case MsgType::kWelcome: m = ReadWelcome(r); break;
    default: known = false; return std::nullopt;
  }
  // Trailing bytes are fields added by a later version: ignored.
  if (!m || !r.ok()) return std::nullopt;
  return m;
}

}  // namespace

void EncodeTo(const Message& msg, std::vector<uint8_t>& out) {
  ByteWriter payload;
  const MsgType type = std::visit([&](const auto& m) { return Write(payload, m); }, msg);
  ByteWriter header;
  header.u8(static_cast<uint8_t>(type));
  header.varint(payload.size());
  out.insert(out.end(), header.data().begin(), header.data().end());
  out.insert(out.end(), payload.data().begin(), payload.data().end());
}

std::vector<uint8_t> Encode(const Message& msg) {
  std::vector<uint8_t> out;
  EncodeTo(msg, out);
  return out;
}

void StreamParser::Append(std::span<const uint8_t> data) {
  // Compact the buffer once more than half of it has been consumed.
  if (pos_ > 0 && pos_ * 2 >= buf_.size()) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ = 0;
  }
  buf_.insert(buf_.end(), data.begin(), data.end());
}

std::optional<Message> StreamParser::Next() {
  while (error_.empty()) {
    ByteReader r(std::span<const uint8_t>(buf_).subspan(pos_));
    uint8_t type = 0;
    uint64_t len = 0;
    if (!r.u8(type) || !r.varint(len)) {
      // A varint header is at most 10 bytes; if we have more than that and
      // still can't parse it, the stream is corrupt rather than incomplete.
      if (buf_.size() - pos_ > 11) error_ = "malformed message header";
      return std::nullopt;
    }
    if (len > kMaxMessageSize) {
      error_ = "message too large";
      return std::nullopt;
    }
    std::span<const uint8_t> payload;
    if (!r.bytes(static_cast<size_t>(len), payload)) return std::nullopt;  // need more bytes

    const size_t consumed = (buf_.size() - pos_) - r.remaining();
    bool known = true;
    auto msg = DecodePayload(static_cast<MsgType>(type), payload, known);
    pos_ += consumed;
    if (!known) {  // a message type from a later version: skip it
      ++skipped_;
      continue;
    }
    if (!msg) {
      pos_ -= consumed;
      error_ = "malformed payload for message type " + std::to_string(type);
      return std::nullopt;
    }
    return msg;
  }
  return std::nullopt;
}

}  // namespace rdacore::proto
