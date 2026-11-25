#include "session/encoder_picture.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace rdacore::session {

void EncoderPicture::Copy(const ImageView& screen, const Rect& r) {
  const size_t row = static_cast<size_t>(r.w) * 4;
  const size_t stride = static_cast<size_t>(width_) * 4;
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    std::memcpy(px_.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(r.x) * 4,
                screen.data + static_cast<size_t>(y) * screen.stride + static_cast<size_t>(r.x) * 4, row);
  }
}

void EncoderPicture::Fill(const Rect& r, uint32_t rgb) {
  const uint32_t px = rgb | 0xff000000u;
  const size_t stride = static_cast<size_t>(width_) * 4;
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    uint8_t* p = px_.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(r.x) * 4;
    for (int32_t x = 0; x < r.w; ++x) std::memcpy(p + x * 4, &px, 4);
  }
}

uint32_t EncoderPicture::ColourAt(int32_t x, int32_t y) const {
  uint32_t px;
  std::memcpy(&px, px_.data() + (static_cast<size_t>(y) * width_ + x) * 4, 4);
  return px & 0x00FFFFFF;
}

bool EncoderPicture::Matches(const std::vector<uint8_t>& other, const Rect& r) const {
  const size_t stride = static_cast<size_t>(width_) * 4;
  for (int32_t y = 0; y < r.h; ++y) {
    const size_t off = static_cast<size_t>(r.y + y) * stride + static_cast<size_t>(r.x) * 4;
    if (std::memcmp(px_.data() + off, other.data() + off, static_cast<size_t>(r.w) * 4) != 0) return false;
  }
  return true;
}

void EncoderPicture::SurroundTaken(const ImageView& screen, const std::vector<tile::TileChange>& taken,
                                   const tile::VideoRegionTracker& areas, int32_t cols, int32_t rows) {
  if (taken.empty()) return;
  const size_t n_tiles = static_cast<size_t>(cols) * rows;
  // Per tile: taken whole, or the parts of it that are.
  std::vector<uint8_t> whole(n_tiles, 0);
  std::unordered_map<size_t, std::vector<Rect>> parts;
  auto index_of = [&](const tile::TileChange& t) { return static_cast<size_t>(t.row) * cols + t.col; };
  for (const auto& t : taken) {
    const int32_t tw = std::min(kTileSize, width_ - t.col * kTileSize);
    const int32_t th = std::min(kTileSize, height_ - t.row * kTileSize);
    if (t.rect.w == tw && t.rect.h == th) {
      whole[index_of(t)] = 1;
    } else {
      parts[index_of(t)].push_back(t.rect);
    }
  }
  const int32_t stride = width_ * 4;
  for (const auto& t : taken) {
    const Rect& r = t.rect;
    const int32_t x0 = std::max(0, r.x - kVideoSurround), x1 = std::min(width_, r.right() + kVideoSurround);
    const int32_t y0 = std::max(0, r.y - kVideoSurround), y1 = std::min(height_, r.bottom() + kVideoSurround);
    for (int32_t y = y0; y < y1; ++y) {
      const bool inside_rows = y >= r.y && y < r.bottom();
      for (int32_t x = x0; x < x1; ++x) {
        if (inside_rows && x == r.x) {
          x = r.right() - 1;  // the rect itself
          continue;
        }
        const int32_t col = x / kTileSize, row = y / kTileSize;
        const size_t idx = static_cast<size_t>(row) * cols + col;
        if (whole[idx] || areas.RegionOf(col, row) >= 0) continue;
        if (const auto it = parts.find(idx); it != parts.end()) {
          bool in_part = false;
          for (const Rect& p : it->second) {
            in_part |= x >= p.x && x < p.right() && y >= p.y && y < p.bottom();
          }
          if (in_part) continue;
        }
        std::memcpy(px_.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4,
                    screen.data + static_cast<size_t>(y) * screen.stride + static_cast<size_t>(x) * 4, 4);
      }
    }
  }
}

void EncoderPicture::KeepScreenNextToVideo(const ImageView& screen, const std::vector<tile::TileChange>& taken,
                                           const std::vector<codec::PaletteTile>& lossless) {
  if (taken.empty() || lossless.empty()) return;
  const int32_t w = width_, h = height_, k = kVideoSurround;
  // The pixels the viewer takes from this picture: the rects taken, less the
  // lossless rects painted over them.
  marks_.assign(static_cast<size_t>(w) * h, 0);
  auto mark = [&](const Rect& r, uint8_t v) {
    const int32_t x0 = std::max(0, r.x), x1 = std::min(w, r.right());
    if (x1 <= x0) return;
    for (int32_t y = std::max(0, r.y); y < std::min(h, r.bottom()); ++y) {
      std::memset(marks_.data() + static_cast<size_t>(y) * w + x0, v, static_cast<size_t>(x1 - x0));
    }
  };
  for (const auto& t : taken) mark(t.rect, 1);
  for (const auto& l : lossless) mark(l.rect, 0);
  auto video_near = [&](int32_t x, int32_t y) {  // a taken pixel within k of (x, y)
    for (int32_t yy = std::max(0, y - k); yy <= std::min(h - 1, y + k); ++yy) {
      const uint8_t* row = marks_.data() + static_cast<size_t>(yy) * w;
      for (int32_t xx = std::max(0, x - k); xx <= std::min(w - 1, x + k); ++xx) {
        if (row[xx]) return true;
      }
    }
    return false;
  };
  const int32_t stride = w * 4;
  for (const auto& l : lossless) {
    const Rect& r = l.rect;
    // Only within k of its edge can a pixel of it be within k of video;
    // most lossless rects have none beside them (the band around it is
    // checked first).
    bool beside = false;
    for (int32_t y = std::max(0, r.y - k); y < std::min(h, r.bottom() + k) && !beside; ++y) {
      const bool band_row = y < r.y || y >= r.bottom();
      for (int32_t x = std::max(0, r.x - k); x < std::min(w, r.right() + k); ++x) {
        if (!band_row && x == r.x) x = r.right();  // skip the rect itself
        if (x < w && marks_[static_cast<size_t>(y) * w + x]) {
          beside = true;
          break;
        }
      }
    }
    if (!beside) continue;
    for (int32_t y = std::max(0, r.y); y < std::min(h, r.bottom()); ++y) {
      const bool edge_row = y < r.y + k || y >= r.bottom() - k;
      for (int32_t x = std::max(0, r.x); x < std::min(w, r.right()); ++x) {
        if (!edge_row && r.w > 2 * k && x == r.x + k) {
          x = r.right() - k - 1;
          continue;
        }
        if (!video_near(x, y)) continue;
        std::memcpy(px_.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4,
                    screen.data + static_cast<size_t>(y) * screen.stride + static_cast<size_t>(x) * 4, 4);
      }
    }
  }
}

}  // namespace rdacore::session
