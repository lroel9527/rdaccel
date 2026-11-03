#pragma once

// Wire protocol messages.
//
// Framing on a byte stream: [type: u8][payload length: varint][payload].
// All messages that belong to one screen update carry the same frame_id; the
// viewer applies a frame only after it has seen FrameEnd for it.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "rdacore/geometry.h"
#include "rdacore/input.h"

namespace rdacore::proto {

// 2: lossless, moves, tile cache. 3: video region streams (VideoData::stream).
// 4: handshake (ViewerHello, Welcome, Hello::min_version), Ping, Goodbye, AppMessage,
//    the message channel; parsers skip unknown message types and ignore
//    trailing fields, so later versions can add both.
inline constexpr uint32_t kProtocolVersion = 4;
// Oldest host stream a viewer still decodes (recordings of version 3 play).
inline constexpr uint32_t kMinHostVersion = 3;
// Oldest viewer a host serves (it must send ViewerHello).
inline constexpr uint32_t kMinViewerVersion = 4;

// Capability bits in Hello / ViewerHello, for features a peer may lack.
inline constexpr uint64_t kCapMessages = 1;  // AppMessage on Channel::kMessage
inline constexpr uint64_t kCapsAll = kCapMessages;

enum class MsgType : uint8_t {
  kHello = 1,
  kFrameBegin = 2,
  kVideoData = 3,
  kFrameEnd = 4,
  kCopyRect = 5,
  kCursorPos = 6,
  kCursorShape = 7,
  kInputEvent = 8,
  kFeedback = 9,
  kLosslessData = 10,
  kKeyframeRequest = 11,
  kViewerHello = 12,
  kPing = 13,
  kGoodbye = 14,
  kAppMessage = 15,
  kWelcome = 16,
};

// Largest screen side the protocol carries (the public API's frame limit).
inline constexpr uint32_t kMaxScreenSide = 16384;

// HEVC only: the project deliberately has no H.264 fallback.
enum class VideoCodec : uint8_t { kHevc = 2 };

// Host -> viewer, first message on the control channel.
struct Hello {
  uint32_t version = kProtocolVersion;
  uint32_t width = 0;   // physical pixels
  uint32_t height = 0;
  VideoCodec codec = VideoCodec::kHevc;
  // Since version 4 (absent in older streams: then min_version = version).
  uint32_t min_version = kMinViewerVersion;  // oldest viewer this host serves
  uint64_t caps = kCapsAll;
  friend bool operator==(const Hello&, const Hello&) = default;
};

// Viewer -> host, first message on the control channel (version 4+).
struct ViewerHello {
  uint32_t version = kProtocolVersion;
  uint32_t min_version = kMinHostVersion;  // oldest host this viewer decodes
  uint64_t caps = kCapsAll;
  std::vector<uint8_t> auth_token;  // opaque, for the host's auth hook
  friend bool operator==(const ViewerHello&, const ViewerHello&) = default;
};

// Host -> viewer, on the control channel: the answer to an accepted
// ViewerHello (the picture's Hello comes with the first frame, which may be
// much later).
struct Welcome {
  uint32_t version = kProtocolVersion;
  uint32_t min_version = kMinViewerVersion;
  uint64_t caps = kCapsAll;
  friend bool operator==(const Welcome&, const Welcome&) = default;
};

// Either direction, on the control channel, about once a second: proves the
// peer is alive when nothing else flows (a static screen, no input).
struct Ping {
  uint64_t time_us = 0;  // sender clock
  friend bool operator==(const Ping&, const Ping&) = default;
};

// Either direction, on the control channel: the sender is closing the session.
enum class GoodbyeReason : uint8_t { kClosed = 0, kIncompatible = 1, kRejected = 2 };
struct Goodbye {
  GoodbyeReason reason = GoodbyeReason::kClosed;
  friend bool operator==(const Goodbye&, const Goodbye&) = default;
};

// Either direction, on the message channel: an application message, or one
// of the library's standard ones (clipboard text). Reliable and ordered.
inline constexpr size_t kMaxAppMessage = 16u << 20;
struct AppMessage {
  uint32_t type = 0;
  std::vector<uint8_t> payload;
  friend bool operator==(const AppMessage&, const AppMessage&) = default;
};

struct FrameBegin {
  uint64_t frame_id = 0;
  uint64_t capture_time_us = 0;  // host clock, used for latency measurement
  friend bool operator==(const FrameBegin&, const FrameBegin&) = default;
};

struct VideoData {
  uint64_t frame_id = 0;
  bool keyframe = false;
  // HEVC parameter sets (VPS, SPS, PPS), raw NAL units without start codes.
  // Sent with every keyframe.
  std::vector<std::vector<uint8_t>> parameter_sets;
  // Tiles of the screen this frame is allowed to update. Empty = whole screen.
  std::vector<Rect> regions;
  // Encoded picture as 4-byte big-endian length-prefixed NAL units, exactly
  // as VideoToolbox emits them (HVCC layout). The Linux encoder converts its
  // Annex B output to this layout so both ends see the same format.
  std::vector<uint8_t> nal_data;
  // 0 = the full-screen stream. Other values are video region streams: a
  // separate HEVC stream (own decoder, own keyframes) whose pictures cover
  // only part of the screen; the picture's top-left goes to (x, y) and
  // `regions` is then relative to the screen as well. A keyframe on a stream
  // id the viewer has not seen, or with new parameter sets, starts it over.
  // Encoded only when non-zero, so version 2 recordings still parse.
  uint8_t stream = 0;
  int32_t x = 0;
  int32_t y = 0;
  friend bool operator==(const VideoData&, const VideoData&) = default;
};

struct FrameEnd {
  uint64_t frame_id = 0;
  friend bool operator==(const FrameEnd&, const FrameEnd&) = default;
};

// Region commands of a frame (tile cache stores and loads, moves) and its
// few-colour tiles coded losslessly; see codec/lossless.h. The viewer applies
// stores, moves and loads before the video picture of the same frame and the
// tiles after it, so they win where both cover a tile. `reset` restarts the
// deflate stream (keyframes).
struct LosslessData {
  uint64_t frame_id = 0;
  bool reset = false;
  std::vector<uint8_t> data;
  friend bool operator==(const LosslessData&, const LosslessData&) = default;
};

// The viewer's tile cache (see tile/tile_cache.h) has this many slots of at
// most kTileSize x kTileSize pixels.
inline constexpr uint32_t kTileCacheSlots = 1024;

// Copy an already-displayed rectangle by (dx, dy). Used for scrolling and
// window moves (M1+).
struct CopyRect {
  uint64_t frame_id = 0;
  Rect src;
  int32_t dx = 0;
  int32_t dy = 0;
  friend bool operator==(const CopyRect&, const CopyRect&) = default;
};

// Cursor messages travel on their own channel (transport::Channel::kCursor),
// so a big video frame never delays the pointer.
struct CursorPos {
  int32_t x = 0;  // pointer position (the hotspot) in screen pixels
  int32_t y = 0;
  bool visible = true;
  uint64_t shape_id = 0;  // 0 = no image known; otherwise a CursorShape sent earlier
  uint64_t time_us = 0;   // host clock
  friend bool operator==(const CursorPos&, const CursorPos&) = default;
};

inline constexpr uint32_t kMaxCursorSize = 256;  // pixels per side

struct CursorShape {
  uint64_t shape_id = 0;
  int32_t hotspot_x = 0;
  int32_t hotspot_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // width * height * 4 bytes, alpha premultiplied. Deflated on the wire.
  std::vector<uint8_t> bgra;
  friend bool operator==(const CursorShape&, const CursorShape&) = default;
};

using InputKind = rdacore::InputKind;
using InputEvent = rdacore::InputEvent;

// Viewer -> host: one entry per presented frame.
struct Feedback {
  uint64_t frame_id = 0;
  uint64_t receive_time_us = 0;  // viewer clock
  uint64_t present_time_us = 0;
  friend bool operator==(const Feedback&, const Feedback&) = default;
};

// Viewer -> host, on the control channel: the viewer cannot decode the
// stream (a decoder error, or it missed the start) and needs a full refresh.
struct KeyframeRequest {
  uint64_t last_frame_id = 0;  // last frame the viewer showed, 0 = none
  friend bool operator==(const KeyframeRequest&, const KeyframeRequest&) = default;
};

using Message = std::variant<Hello, FrameBegin, VideoData, FrameEnd, CopyRect, CursorPos,
                             CursorShape, InputEvent, Feedback, LosslessData, KeyframeRequest,
                             ViewerHello, Ping, Goodbye, AppMessage, Welcome>;

// Serializes one message including its type and length header.
std::vector<uint8_t> Encode(const Message& msg);

// Appends one serialized message to `out`.
void EncodeTo(const Message& msg, std::vector<uint8_t>& out);

// Incremental parser for a byte stream that may deliver messages in arbitrary
// chunks. Feed bytes with Append(), then call Next() until it returns nullopt.
class StreamParser {
 public:
  // Rejects any single message larger than this to bound memory use.
  static constexpr size_t kMaxMessageSize = 64u << 20;

  void Append(std::span<const uint8_t> data);

  // Returns the next complete message, or nullopt if more bytes are needed.
  // After a protocol error, error() is set and Next() always returns nullopt.
  std::optional<Message> Next();

  const std::string& error() const { return error_; }
  // Messages of unknown type skipped so far.
  uint64_t skipped() const { return skipped_; }

 private:
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  std::string error_;
  uint64_t skipped_ = 0;
};

}  // namespace rdacore::proto
