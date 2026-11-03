#pragma once

// The cursor channel (transport::Channel::kCursor): positions and images of
// the mouse cursor, separate from the screen picture.
//
// Each image is sent once, deflated, under a 64-bit id derived from its
// pixels; later positions refer to it by id. Both ends keep the last
// kCursorCacheSize images in the order they were sent, so the sender knows
// exactly which images the viewer still has and an image that comes back
// (the text beam and the arrow alternate all the time) costs nothing.

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "proto/messages.h"
#include "rdacore/cursor.h"
#include "transport/transport.h"

namespace rdacore::session {

inline constexpr size_t kCursorCacheSize = 64;

// 64-bit content id of an image, never 0.
uint64_t CursorImageId(const CursorImage& img);

// `img` resized by `factor` (e.g. 0.5 when the screen is captured at half
// its size), hotspot included, at most 256 x 256 and at least 1 x 1.
// Shrinking averages the covered pixels (premultiplied alpha, so edges stay
// clean); enlarging repeats them.
CursorImage ScaleCursorImage(const CursorImage& img, double factor);

class CursorSender {
 public:
  explicit CursorSender(transport::ITransport* transport) : transport_(transport) {}

  // Thread-safe. Nothing is sent if neither the position nor the image
  // changed. An image larger than proto::kMaxCursorSize is ignored.
  void SetPosition(int32_t x, int32_t y, bool visible, uint64_t time_us);
  void SetImage(const CursorImage& img);

  struct Stats {
    uint64_t positions = 0;  // CursorPos messages
    uint64_t images = 0;     // CursorShape messages
    uint64_t bytes = 0;
  };
  Stats stats() const;

 private:
  void SendPosLocked();
  void SendLocked(const proto::Message& m);

  transport::ITransport* transport_;
  mutable std::mutex mu_;
  proto::CursorPos pos_;
  bool have_pos_ = false;
  std::deque<uint64_t> sent_;  // ids the viewer has, oldest first
  Stats stats_;
};

class CursorReceiver {
 public:
  void OnShape(proto::CursorShape&& s);
  // Fills `out`; out.image points into this object until the next call.
  void OnPos(const proto::CursorPos& p, CursorState& out);

 private:
  std::unordered_map<uint64_t, CursorImage> images_;
  std::deque<uint64_t> order_;
  uint64_t last_id_ = 0;
};

}  // namespace rdacore::session
