#include "session/cursor_channel.h"

#include <algorithm>
#include <cmath>

namespace rdacore::session {

CursorImage ScaleCursorImage(const CursorImage& img, double factor) {
  if (img.width <= 0 || img.height <= 0 || !(factor > 0)) return img;
  CursorImage out;
  out.width = std::clamp(static_cast<int32_t>(std::lround(img.width * factor)), 1, 256);
  out.height = std::clamp(static_cast<int32_t>(std::lround(img.height * factor)), 1, 256);
  if (out.width == img.width && out.height == img.height) return img;
  const double sx = static_cast<double>(img.width) / out.width;
  const double sy = static_cast<double>(img.height) / out.height;
  out.hotspot_x = std::clamp(static_cast<int32_t>(img.hotspot_x / sx), 0, out.width - 1);
  out.hotspot_y = std::clamp(static_cast<int32_t>(img.hotspot_y / sy), 0, out.height - 1);
  out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
  for (int32_t y = 0; y < out.height; ++y) {
    // Source rows [y0, y1) cover this row (at least one).
    const auto y0 = static_cast<int32_t>(y * sy);
    const int32_t y1 = std::max(y0 + 1, std::min(img.height, static_cast<int32_t>((y + 1) * sy)));
    for (int32_t x = 0; x < out.width; ++x) {
      const auto x0 = static_cast<int32_t>(x * sx);
      const int32_t x1 = std::max(x0 + 1, std::min(img.width, static_cast<int32_t>((x + 1) * sx)));
      uint32_t sum[4] = {0, 0, 0, 0};
      for (int32_t v = y0; v < y1; ++v) {
        const uint8_t* p = &img.bgra[(static_cast<size_t>(v) * img.width + x0) * 4];
        for (int32_t u = x0; u < x1; ++u, p += 4) {
          for (int k = 0; k < 4; ++k) sum[k] += p[k];
        }
      }
      const auto n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
      uint8_t* d = &out.bgra[(static_cast<size_t>(y) * out.width + x) * 4];
      for (int k = 0; k < 4; ++k) d[k] = static_cast<uint8_t>((sum[k] + n / 2) / n);
    }
  }
  return out;
}

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

}  // namespace rdacore::session
