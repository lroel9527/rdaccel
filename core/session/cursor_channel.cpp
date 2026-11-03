#include "session/cursor_channel.h"

#include <algorithm>

namespace rdaccel::session {

uint64_t CursorImageId(const CursorImage& img) {
  // FNV-1a over the geometry and the pixels; cursors are small.
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&h](uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xff;
      h *= 0x100000001b3ull;
    }
  };
  mix(static_cast<uint32_t>(img.width));
  mix(static_cast<uint32_t>(img.height));
  mix(static_cast<uint32_t>(img.hotspot_x));
  mix(static_cast<uint32_t>(img.hotspot_y));
  for (uint8_t b : img.bgra) {
    h ^= b;
    h *= 0x100000001b3ull;
  }
  return h ? h : 1;
}

void CursorSender::SendLocked(const proto::Message& m) {
  const auto bytes = proto::Encode(m);
  stats_.bytes += bytes.size();
  if (transport_) transport_->Send(transport::Channel::kCursor, bytes);
}

void CursorSender::SendPosLocked() {
  have_pos_ = true;
  ++stats_.positions;
  SendLocked(pos_);
}

void CursorSender::SetPosition(int32_t x, int32_t y, bool visible, uint64_t time_us) {
  std::lock_guard<std::mutex> lock(mu_);
  if (have_pos_ && x == pos_.x && y == pos_.y && visible == pos_.visible) return;
  pos_.x = x;
  pos_.y = y;
  pos_.visible = visible;
  pos_.time_us = time_us;
  SendPosLocked();
}

void CursorSender::SetImage(const CursorImage& img) {
  if (img.width <= 0 || img.height <= 0 || img.width > static_cast<int32_t>(proto::kMaxCursorSize) ||
      img.height > static_cast<int32_t>(proto::kMaxCursorSize) ||
      img.bgra.size() != static_cast<size_t>(img.width) * img.height * 4) {
    return;
  }
  const uint64_t id = CursorImageId(img);
  std::lock_guard<std::mutex> lock(mu_);
  if (id == pos_.shape_id) return;
  if (std::find(sent_.begin(), sent_.end(), id) == sent_.end()) {
    proto::CursorShape s;
    s.shape_id = id;
    s.hotspot_x = img.hotspot_x;
    s.hotspot_y = img.hotspot_y;
    s.width = static_cast<uint32_t>(img.width);
    s.height = static_cast<uint32_t>(img.height);
    s.bgra = img.bgra;
    ++stats_.images;
    SendLocked(s);
    sent_.push_back(id);
    if (sent_.size() > kCursorCacheSize) sent_.pop_front();
  }
  pos_.shape_id = id;
  // Tell the viewer right away, with the last known position.
  if (have_pos_) SendPosLocked();
}

CursorSender::Stats CursorSender::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

void CursorReceiver::OnShape(proto::CursorShape&& s) {
  if (!images_.count(s.shape_id)) {
    order_.push_back(s.shape_id);
    // Same rule as the sender: evict the oldest received.
    if (order_.size() > kCursorCacheSize) {
      images_.erase(order_.front());
      order_.pop_front();
    }
  }
  CursorImage& img = images_[s.shape_id];
  img.width = static_cast<int32_t>(s.width);
  img.height = static_cast<int32_t>(s.height);
  img.hotspot_x = s.hotspot_x;
  img.hotspot_y = s.hotspot_y;
  img.bgra = std::move(s.bgra);
}

void CursorReceiver::OnPos(const proto::CursorPos& p, CursorState& out) {
  out.x = p.x;
  out.y = p.y;
  out.visible = p.visible;
  out.time_us = p.time_us;
  out.image_id = p.shape_id;
  out.image_changed = p.shape_id != last_id_;
  last_id_ = p.shape_id;
  auto it = images_.find(p.shape_id);
  out.image = it == images_.end() ? nullptr : &it->second;
}

}  // namespace rdaccel::session
