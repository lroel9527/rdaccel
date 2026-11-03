#include "tile/motion.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace rdacore::tile {
namespace {

constexpr uint64_t kBase = 0x100000001b3ull;  // any odd multiplier works
constexpr int kMinTransitions = 6;             // a probe row must have some detail
constexpr size_t kMaxHitsPerProbe = 16;        // more = repetitive content, no signal

inline uint32_t Px(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v & 0x00ffffffu;
}

inline const uint8_t* At(const ImageView& img, int32_t x, int32_t y) {
  return img.data + static_cast<size_t>(y) * img.stride + static_cast<size_t>(x) * 4;
}

uint64_t HashRow(const uint8_t* p) {
  uint64_t h = 0;
  for (int32_t i = 0; i < kTileSize; ++i) h = h * kBase + Px(p + i * 4);
  return h;
}

struct Probe {
  uint64_t hash;
  int32_t x, y;  // where the row is in the current frame
  size_t change;
  std::vector<std::pair<int32_t, int32_t>> hits;  // positions in the previous frame
};

bool SameRect(const ImageView& a, const Rect& ra, const ImageView& b, int32_t bx, int32_t by) {
  const size_t row = static_cast<size_t>(ra.w) * 4;
  for (int32_t y = 0; y < ra.h; ++y) {
    if (std::memcmp(At(a, ra.x, ra.y + y), At(b, bx, by + y), row) != 0) return false;
  }
  return true;
}

// Merges disjoint rects into fewer ones: horizontal runs first, then runs
// with the same columns stacked on top of each other.
std::vector<Rect> MergeRects(std::vector<Rect> rects) {
  std::sort(rects.begin(), rects.end(),
            [](const Rect& a, const Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
  std::vector<Rect> runs;
  for (const Rect& r : rects) {
    if (!runs.empty() && runs.back().y == r.y && runs.back().h == r.h &&
        runs.back().right() == r.x) {
      runs.back().w += r.w;
    } else {
      runs.push_back(r);
    }
  }
  std::sort(runs.begin(), runs.end(), [](const Rect& a, const Rect& b) {
    return a.x != b.x ? a.x < b.x : a.w != b.w ? a.w < b.w : a.y < b.y;
  });
  std::vector<Rect> out;
  for (const Rect& r : runs) {
    if (!out.empty() && out.back().x == r.x && out.back().w == r.w && out.back().bottom() == r.y) {
      out.back().h += r.h;
    } else {
      out.push_back(r);
    }
  }
  return out;
}

}  // namespace

std::vector<Move> DetectMoves(const ImageView& prev, const ImageView& cur,
                              std::span<const TileChange> changes, std::vector<bool>& moved,
                              std::vector<std::vector<Rect>>& residual,
                              const MotionOptions& opts) {
  moved.assign(changes.size(), false);
  residual.assign(changes.size(), {});
  if (changes.size() < opts.min_changed_tiles || prev.width != cur.width ||
      prev.height != cur.height || cur.width < kTileSize) {
    return {};
  }

  // 1. Probes.
  std::vector<Probe> probes;
  for (size_t i = 0; i < changes.size(); ++i) {
    const Rect& r = changes[i].rect;
    if (r.w != kTileSize) continue;
    int best = -1, best_y = 0;
    for (int32_t y = r.y; y < r.bottom(); ++y) {
      const uint8_t* p = At(cur, r.x, y);
      int n = 0;
      for (int32_t x = 1; x < r.w; ++x) n += Px(p + x * 4) != Px(p + (x - 1) * 4);
      if (n > best) {
        best = n;
        best_y = y;
      }
    }
    if (best < kMinTransitions) continue;
    probes.push_back({HashRow(At(cur, r.x, best_y)), r.x, best_y, i, {}});
  }
  if (probes.size() < 2) return {};

  // 2. One rolling-hash pass over the previous frame. A bit filter rejects
  // almost every position before the sorted probe list is searched.
  std::vector<std::pair<uint64_t, size_t>> sorted;
  sorted.reserve(probes.size());
  for (size_t i = 0; i < probes.size(); ++i) sorted.emplace_back(probes[i].hash, i);
  std::sort(sorted.begin(), sorted.end());
  constexpr size_t kFilterBits = 1 << 16;
  std::vector<uint64_t> filter(kFilterBits / 64, 0);
  auto fbit = [](uint64_t h) { return static_cast<size_t>((h >> 20) & (kFilterBits - 1)); };
  for (const auto& [h, i] : sorted) filter[fbit(h) / 64] |= 1ull << (fbit(h) % 64);

  uint64_t base_pow = 1;  // kBase^(kTileSize-1)
  for (int32_t i = 1; i < kTileSize; ++i) base_pow *= kBase;
  for (int32_t y = 0; y < prev.height; ++y) {
    const uint8_t* row = At(prev, 0, y);
    uint64_t h = HashRow(row);
    for (int32_t x = 0;; ++x) {
      const size_t b = fbit(h);
      if (filter[b / 64] & (1ull << (b % 64))) {
        auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(h, size_t{0}));
        for (; it != sorted.end() && it->first == h; ++it) {
          auto& hits = probes[it->second].hits;
          if (hits.size() <= kMaxHitsPerProbe) hits.emplace_back(x, y);
        }
      }
      if (x + kTileSize >= prev.width) break;
      h = (h - Px(row + x * 4) * base_pow) * kBase + Px(row + (x + kTileSize) * 4);
    }
  }

  // 3. Votes, then exact verification per tile.
  std::map<std::pair<int32_t, int32_t>, int> votes;
  for (const auto& p : probes) {
    if (p.hits.size() > kMaxHitsPerProbe) continue;
    for (const auto& [hx, hy] : p.hits) {
      if (hx == p.x && hy == p.y) continue;  // not a move
      ++votes[{p.x - hx, p.y - hy}];
    }
  }
  std::vector<std::pair<int, std::pair<int32_t, int32_t>>> ranked;
  for (const auto& [v, n] : votes) {
    if (n >= 2) ranked.push_back({n, v});
  }
  std::sort(ranked.begin(), ranked.end(), std::greater<>());
  if (ranked.size() > static_cast<size_t>(opts.max_vectors)) ranked.resize(opts.max_vectors);

  // Pieces of the screen each vector explains; merged into moves at the end.
  std::vector<std::vector<Rect>> pieces(ranked.size());
  for (size_t k = 0; k < ranked.size(); ++k) {
    const auto [dx, dy] = ranked[k].second;
    for (size_t i = 0; i < changes.size(); ++i) {
      if (moved[i]) continue;
      const Rect& r = changes[i].rect;
      const int32_t sx = r.x - dx, sy = r.y - dy;
      if (sx < 0 || sy < 0 || sx + r.w > prev.width || sy + r.h > prev.height) continue;
      if (!SameRect(cur, r, prev, sx, sy)) continue;
      moved[i] = true;
      pieces[k].push_back(r);
    }
  }

  // 4. Split the remaining tiles into 4-pixel-wide strips and classify each
  // strip row, for the strongest vector only. Strips (rather than whole rows)
  // also catch the left and right edges of a dragged window; narrow ones keep
  // what is left to code at the edge itself.
  if (!ranked.empty()) {
    const auto [dx, dy] = ranked.front().second;
    constexpr int32_t kStrip = 4;
    std::vector<Rect>& partial = pieces.front();
    std::vector<char> kind;  // per strip row: 'M' moved, 'U' unchanged, 'X' new
    for (size_t i = 0; i < changes.size(); ++i) {
      if (moved[i]) continue;
      const Rect& r = changes[i].rect;
      const int32_t strips = (r.w + kStrip - 1) / kStrip;
      kind.assign(static_cast<size_t>(strips) * r.h, 'X');
      int n_moved = 0, n_new = 0;
      for (int32_t s = 0; s < strips; ++s) {
        const int32_t x0 = r.x + s * kStrip;
        const int32_t sw = std::min(kStrip, r.right() - x0);
        const int32_t sx = x0 - dx;
        const size_t bytes = static_cast<size_t>(sw) * 4;
        for (int32_t y = 0; y < r.h; ++y) {
          const int32_t cy = r.y + y, sy = cy - dy;
          const bool m = sx >= 0 && sx + sw <= prev.width && sy >= 0 && sy < prev.height &&
                         std::memcmp(At(cur, x0, cy), At(prev, sx, sy), bytes) == 0;
          const bool u = std::memcmp(At(cur, x0, cy), At(prev, x0, cy), bytes) == 0;
          kind[static_cast<size_t>(s) * r.h + y] = m && u ? 'B' : m ? 'M' : u ? 'U' : 'X';
        }
        // Rows that match both ways (blank rows) join the run next to them,
        // so a static header stays static and a moving page stays one move.
        char* col = &kind[static_cast<size_t>(s) * r.h];
        for (int32_t y = 0; y < r.h;) {
          if (col[y] != 'B') {
            ++y;
            continue;
          }
          int32_t e = y;
          while (e < r.h && col[e] == 'B') ++e;
          char fill = 'U';
          if (y > 0 && col[y - 1] != 'X') {
            fill = col[y - 1];
          } else if (e < r.h && (col[e] == 'M' || col[e] == 'U')) {
            fill = col[e];
          }
          std::fill(col + y, col + e, fill);
          y = e;
        }
        for (int32_t y = 0; y < r.h; ++y) {
          n_moved += col[y] == 'M';
          n_new += col[y] == 'X';
        }
      }
      if (n_moved == 0 || n_new * 2 > strips * r.h) continue;  // not worth splitting
      moved[i] = true;
      std::vector<Rect> res;
      for (int32_t s = 0; s < strips; ++s) {
        const int32_t x0 = r.x + s * kStrip;
        const int32_t sw = std::min(kStrip, r.right() - x0);
        const char* col = &kind[static_cast<size_t>(s) * r.h];
        for (int32_t y = 0; y < r.h;) {
          int32_t e = y;
          while (e < r.h && col[e] == col[y]) ++e;
          const Rect run{x0, r.y + y, sw, e - y};
          if (col[y] == 'M') partial.push_back(run);
          if (col[y] == 'X') res.push_back(run);
          y = e;
        }
      }
      // Join strips that ended up with the same rows.
      std::sort(res.begin(), res.end(), [](const Rect& a, const Rect& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
      });
      for (const Rect& run : res) {
        auto& out = residual[i];
        if (!out.empty() && out.back().y == run.y && out.back().h == run.h &&
            out.back().right() == run.x) {
          out.back().w += run.w;
        } else {
          out.push_back(run);
        }
      }
    }

    // Unchanged tiles inside the moving area (blank page, flat window body)
    // also match the shifted picture. Copying them changes nothing, but it
    // lets the pieces around them join into a few large moves.
    if (!partial.empty()) {
      Rect box = partial.front();
      for (const Rect& r : partial) {
        const int32_t x1 = std::max(box.right(), r.right()), y1 = std::max(box.bottom(), r.bottom());
        box.x = std::min(box.x, r.x);
        box.y = std::min(box.y, r.y);
        box.w = x1 - box.x;
        box.h = y1 - box.y;
      }
      std::vector<uint8_t> is_changed(static_cast<size_t>((cur.width + kTileSize - 1) / kTileSize) *
                                      ((cur.height + kTileSize - 1) / kTileSize));
      const int32_t cols = (cur.width + kTileSize - 1) / kTileSize;
      for (const auto& c : changes) is_changed[static_cast<size_t>(c.row) * cols + c.col] = 1;
      for (int32_t row = box.y / kTileSize; row <= (box.bottom() - 1) / kTileSize; ++row) {
        for (int32_t col = box.x / kTileSize; col <= (box.right() - 1) / kTileSize; ++col) {
          if (is_changed[static_cast<size_t>(row) * cols + col]) continue;
          Rect r{col * kTileSize, row * kTileSize, kTileSize, kTileSize};
          r.w = std::min(r.w, cur.width - r.x);
          r.h = std::min(r.h, cur.height - r.y);
          const int32_t sx = r.x - dx, sy = r.y - dy;
          if (sx < 0 || sy < 0 || sx + r.w > prev.width || sy + r.h > prev.height) continue;
          if (SameRect(cur, r, prev, sx, sy)) partial.push_back(r);
        }
      }
    }
  }

  std::vector<Move> moves;
  for (size_t k = 0; k < ranked.size(); ++k) {
    const auto [dx, dy] = ranked[k].second;
    for (const Rect& r : MergeRects(std::move(pieces[k]))) moves.push_back({r, dx, dy});
  }
  return moves;
}

void ApplyMoves(std::span<const Move> moves, uint8_t* bgra, int32_t width, int32_t height,
                std::vector<uint8_t>& scratch) {
  if (moves.empty()) return;
  const size_t stride = static_cast<size_t>(width) * 4;
  scratch.assign(bgra, bgra + stride * height);
  for (const Move& m : moves) {
    const Rect s = m.src();
    // Callers validate moves; this only guards memory.
    if (s.x < 0 || s.y < 0 || s.right() > width || s.bottom() > height || m.dst.x < 0 ||
        m.dst.y < 0 || m.dst.right() > width || m.dst.bottom() > height || m.dst.empty()) {
      continue;
    }
    for (int32_t y = 0; y < m.dst.h; ++y) {
      std::memcpy(bgra + (m.dst.y + y) * stride + static_cast<size_t>(m.dst.x) * 4,
                  scratch.data() + (s.y + y) * stride + static_cast<size_t>(s.x) * 4,
                  static_cast<size_t>(m.dst.w) * 4);
    }
  }
}

}  // namespace rdacore::tile
