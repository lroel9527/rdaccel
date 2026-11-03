#include "tile/video_region.h"

#include <algorithm>
#include <bit>

namespace rdaccel::tile {
namespace {

bool Contains(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
         inner.bottom() <= outer.bottom();
}

Rect Union(const Rect& a, const Rect& b) {
  const int32_t x = std::min(a.x, b.x), y = std::min(a.y, b.y);
  return {x, y, std::max(a.right(), b.right()) - x, std::max(a.bottom(), b.bottom()) - y};
}

}  // namespace

void VideoRegionTracker::Reset(int32_t width, int32_t height) {
  width_ = width;
  height_ = height;
  cols_ = (width + kTileSize - 1) / kTileSize;
  rows_ = (height + kTileSize - 1) / kTileSize;
  frame_ = 0;
  history_.assign(static_cast<size_t>(cols_) * rows_, 0);
  candidates_.clear();
  regions_.clear();
}

void VideoRegionTracker::Clear() {
  std::fill(history_.begin(), history_.end(), 0);
  candidates_.clear();
  regions_.clear();
}

VideoRegion VideoRegionTracker::Close(int index) {
  VideoRegion r = regions_[static_cast<size_t>(index)];
  regions_.erase(regions_.begin() + index);
  // Its tiles must earn their way back in.
  for (int32_t row = r.rect.y / kTileSize; row * kTileSize < r.rect.bottom(); ++row) {
    for (int32_t col = r.rect.x / kTileSize; col * kTileSize < r.rect.right(); ++col) {
      history_[static_cast<size_t>(row) * cols_ + col] = 0;
    }
  }
  return r;
}

int VideoRegionTracker::RegionOf(int32_t col, int32_t row) const {
  const int32_t x = col * kTileSize, y = row * kTileSize;
  for (size_t i = 0; i < regions_.size(); ++i) {
    const Rect& r = regions_[i].rect;
    if (x >= r.x && x < r.right() && y >= r.y && y < r.bottom()) return static_cast<int>(i);
  }
  return -1;
}

Rect VideoRegionTracker::ToPixels(const Rect& t) const {
  Rect r{t.x * kTileSize, t.y * kTileSize, t.w * kTileSize, t.h * kTileSize};
  r.w = std::min(r.w, width_ - r.x);
  r.h = std::min(r.h, height_ - r.y);
  return r;
}

uint8_t VideoRegionTracker::NextStream() {
  const uint8_t s = next_stream_;
  next_stream_ = next_stream_ == 255 ? 1 : next_stream_ + 1;
  return s;
}

std::vector<VideoRegion> VideoRegionTracker::Update(const std::vector<uint8_t>& busy,
                                                    const std::vector<uint8_t>& changed) {
  ++frame_;
  const size_t n = history_.size();
  const uint64_t mask = p_.window >= 64 ? ~0ull : (1ull << p_.window) - 1;
  for (size_t i = 0; i < n; ++i) {
    history_[i] = ((history_[i] << 1) | (i < busy.size() && busy[i] ? 1 : 0)) & mask;
  }
  for (auto& reg : regions_) {
    for (int32_t row = reg.rect.y / kTileSize; row * kTileSize < reg.rect.bottom(); ++row) {
      for (int32_t col = reg.rect.x / kTileSize; col * kTileSize < reg.rect.right(); ++col) {
        const size_t i = static_cast<size_t>(row) * cols_ + col;
        if (i < changed.size() && changed[i]) reg.last_change = frame_;
      }
    }
  }

  // Connected groups of active tiles.
  std::vector<uint8_t> active(n, 0);
  for (size_t i = 0; i < n; ++i) {
    active[i] = std::popcount(history_[i]) >= p_.min_busy;
  }
  std::vector<Rect> found;  // tile units
  std::vector<int32_t> stack;
  for (int32_t start = 0; start < static_cast<int32_t>(n); ++start) {
    if (!active[static_cast<size_t>(start)]) continue;
    int32_t x0 = cols_, y0 = rows_, x1 = -1, y1 = -1, count = 0;
    active[static_cast<size_t>(start)] = 0;
    stack.assign(1, start);
    while (!stack.empty()) {
      const int32_t i = stack.back();
      stack.pop_back();
      const int32_t col = i % cols_, row = i / cols_;
      ++count;
      x0 = std::min(x0, col);
      y0 = std::min(y0, row);
      x1 = std::max(x1, col);
      y1 = std::max(y1, row);
      for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
          const int32_t c = col + dx, r = row + dy;
          if (c < 0 || r < 0 || c >= cols_ || r >= rows_) continue;
          const size_t j = static_cast<size_t>(r) * cols_ + c;
          if (active[j]) {
            active[j] = 0;
            stack.push_back(static_cast<int32_t>(j));
          }
        }
      }
    }
    Rect t{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    // Encoders want even dimensions; only a column or row clipped by an odd
    // screen edge can be odd, so drop it.
    if (ToPixels(t).w % 2 != 0) --t.w;
    if (ToPixels(t).h % 2 != 0) --t.h;
    if (t.w < p_.min_tiles_w || t.h < p_.min_tiles_h) continue;
    if (count < p_.min_density * static_cast<double>(t.w) * t.h) continue;
    found.push_back(t);
  }

  std::vector<Candidate> next_candidates;
  for (const Rect& t : found) {
    const Rect px = ToPixels(t);
    bool handled = false;
    for (size_t k = 0; k < regions_.size() && !handled; ++k) {
      VideoRegion& reg = regions_[k];
      if (Contains(reg.rect, px)) {
        handled = true;
      } else if (reg.rect.intersects(px)) {
        handled = true;
        if (frame_ - reg.created < static_cast<uint64_t>(p_.min_resize_gap)) continue;
        Rect u = Union(reg.rect, px);
        // Absorb any other area the union now overlaps.
        for (size_t j = 0; j < regions_.size(); ++j) {
          if (j != k && regions_[j].rect.intersects(u)) u = Union(u, regions_[j].rect);
        }
        VideoRegion grown{NextStream(), u, frame_, frame_};
        std::erase_if(regions_, [&](const VideoRegion& r) { return r.rect.intersects(u); });
        regions_.push_back(grown);
      }
    }
    if (handled) continue;
    Candidate c{t, 1};
    for (const auto& old : candidates_) {
      if (old.tiles.intersects(t)) c.seen = std::max(c.seen, old.seen + 1);
    }
    if (c.seen >= p_.confirm && static_cast<int32_t>(regions_.size()) < p_.max_regions) {
      regions_.push_back({NextStream(), px, frame_, frame_});
    } else {
      next_candidates.push_back(c);
    }
  }
  candidates_ = std::move(next_candidates);

  std::vector<VideoRegion> closed;
  for (size_t k = 0; k < regions_.size();) {
    if (frame_ - regions_[k].last_change >= static_cast<uint64_t>(p_.linger)) {
      closed.push_back(Close(static_cast<int>(k)));
    } else {
      ++k;
    }
  }
  return closed;
}

}  // namespace rdaccel::tile
