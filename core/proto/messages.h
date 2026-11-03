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

#include "rdaccel/geometry.h"
#include "rdaccel/input.h"

namespace rdaccel::proto {

inline constexpr uint32_t kProtocolVersion = 1;

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
};

// HEVC only: the project deliberately has no H.264 fallback.
enum class VideoCodec : uint8_t { kHevc = 2 };

struct Hello {
  uint32_t version = kProtocolVersion;
  uint32_t width = 0;   // physical pixels
  uint32_t height = 0;
  VideoCodec codec = VideoCodec::kHevc;
  friend bool operator==(const Hello&, const Hello&) = default;
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
  friend bool operator==(const VideoData&, const VideoData&) = default;
};

struct FrameEnd {
  uint64_t frame_id = 0;
  friend bool operator==(const FrameEnd&, const FrameEnd&) = default;
};

// Copy an already-displayed rectangle by (dx, dy). Used for scrolling and
// window moves (M1+).
struct CopyRect {
  uint64_t frame_id = 0;
  Rect src;
  int32_t dx = 0;
  int32_t dy = 0;
  friend bool operator==(const CopyRect&, const CopyRect&) = default;
};

struct CursorPos {
  int32_t x = 0;
  int32_t y = 0;
  bool visible = true;
  friend bool operator==(const CursorPos&, const CursorPos&) = default;
};

struct CursorShape {
  uint64_t shape_id = 0;
  int32_t hotspot_x = 0;
  int32_t hotspot_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> bgra;  // width * height * 4 bytes
  friend bool operator==(const CursorShape&, const CursorShape&) = default;
};

using InputKind = rdaccel::InputKind;
using InputEvent = rdaccel::InputEvent;

// Viewer -> host: one entry per presented frame.
struct Feedback {
  uint64_t frame_id = 0;
  uint64_t receive_time_us = 0;  // viewer clock
  uint64_t present_time_us = 0;
  friend bool operator==(const Feedback&, const Feedback&) = default;
};

using Message = std::variant<Hello, FrameBegin, VideoData, FrameEnd, CopyRect, CursorPos,
                             CursorShape, InputEvent, Feedback>;

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

 private:
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace rdaccel::proto
