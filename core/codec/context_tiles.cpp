#include "codec/context_tiles.h"

#include <algorithm>
#include <array>

namespace rdacore::codec {
namespace {

// 2^19 contexts of indices or levels (three bits of palette size class),
// then those of the colour within a level.
constexpr uint32_t kLevelContexts = 1u << 19;
constexpr uint32_t kWithinContexts = 2 * 16 * 25;
constexpr uint32_t kContexts = kLevelContexts + kWithinContexts;
constexpr int kSymbols = 16;
constexpr int kIncrement = 24;
constexpr int kLimit = 4000;  // of a context's total: then every count is halved
constexpr uint32_t kTop = 1u << 24;

// A range coder as in LZMA (carry propagation through a cached byte), for
// symbols given as a slice [cum, cum + freq) of [0, total), total < 2^16.
class RangeEncoder {
 public:
  explicit RangeEncoder(std::vector<uint8_t>& out) : out_(out) {}
  void Encode(uint32_t cum, uint32_t freq, uint32_t total) {
    const uint32_t r = range_ / total;
    low_ += static_cast<uint64_t>(r) * cum;
    range_ = r * freq;
    while (range_ < kTop) {
      range_ <<= 8;
      ShiftLow();
    }
  }
  void Finish() {
    for (int i = 0; i < 5; ++i) ShiftLow();
  }

 private:
  void ShiftLow() {
    if (static_cast<uint32_t>(low_) < 0xff000000u || (low_ >> 32) != 0) {
      const auto carry = static_cast<uint8_t>(low_ >> 32);
      uint8_t byte = cache_;
      do {
        out_.push_back(static_cast<uint8_t>(byte + carry));
        byte = 0xff;
      } while (--cache_size_ != 0);
      cache_ = static_cast<uint8_t>(low_ >> 24);
    }
    ++cache_size_;
    low_ = (low_ & 0x00ffffffu) << 8;
  }

  std::vector<uint8_t>& out_;
  uint64_t low_ = 0;
  uint32_t range_ = 0xffffffffu;
  uint8_t cache_ = 0;
  uint64_t cache_size_ = 1;
};

class RangeDecoder {
 public:
  explicit RangeDecoder(std::span<const uint8_t> in) : in_(in) {
    for (int i = 0; i < 5; ++i) code_ = (code_ << 8) | Next();
  }
  // The value to look up among the cumulative counts.
  uint32_t Peek(uint32_t total) {
    r_ = range_ / total;
    return std::min(code_ / r_, total - 1);
  }
  void Take(uint32_t cum, uint32_t freq) {
    code_ -= r_ * cum;
    range_ = r_ * freq;
    while (range_ < kTop) {
      code_ = (code_ << 8) | Next();
      range_ <<= 8;
    }
  }
  // More bytes were asked for than a valid block has beyond its end.
  bool overrun() const { return over_ > 4; }

 private:
  uint32_t Next() {
    if (pos_ < in_.size()) return in_[pos_++];
    ++over_;
    return 0;
  }
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  size_t over_ = 0;
  uint32_t code_ = 0;
  uint32_t range_ = 0xffffffffu;
  uint32_t r_ = 1;
};

// Palette size class, and a neighbour's index in four steps.
inline uint32_t SizeClass(int n) { return n <= 2 ? 0 : n <= 4 ? 1 : n <= 8 ? 2 : 3; }

struct Steps {
  std::array<uint8_t, kSymbols> of{};
  explicit Steps(int n) {
    for (int r = 1; r < kSymbols; ++r) {
      of[r] = static_cast<uint8_t>(1 + std::min(2, (r - 1) * 3 / std::max(1, n - 1)));
    }
  }
};

// A tile of more than 16 colours: the level of each index, and each level's
// first index and number of colours.
struct Levels {
  std::array<uint8_t, kContextTileMaxColors> of{};
  std::array<uint8_t, kSymbols + 1> first{};
  explicit Levels(int n) {
    for (int v = 0; v < n; ++v) of[static_cast<size_t>(v)] = static_cast<uint8_t>(v * kSymbols / n);
    for (int l = 0; l <= kSymbols; ++l) first[static_cast<size_t>(l)] = static_cast<uint8_t>((l * n + kSymbols - 1) / kSymbols);
  }
  int size(int level) const { return first[static_cast<size_t>(level) + 1] - first[static_cast<size_t>(level)]; }
};

// Calls fn(context, position, left, above) for each pixel in coding order;
// `idx` holds the indices coded so far (fn fills in the pixel's own before
// returning). `level` maps an index to what the context is made of: the
// index itself up to 16 colours, its level beyond.
template <class Level, class Fn>
bool ForEachPixel(const Rect& rect, uint32_t size_class, const Steps& step, const uint8_t* idx, Level&& level, Fn&& fn) {
  const int32_t w = rect.w, h = rect.h;
  for (int32_t y = 0; y < h; ++y) {
    const uint8_t* row = idx + static_cast<size_t>(y) * w;
    const uint8_t* up = y > 0 ? row - w : nullptr;
    const uint8_t* up2 = y > 1 ? row - 2 * w : nullptr;
    for (int32_t x = 0; x < w; ++x) {
      const uint32_t left_index = x > 0 ? row[x - 1] : 0;
      const uint32_t above_index = up ? up[x] : 0;
      const uint32_t left = level(left_index);
      const uint32_t above = level(above_index);
      const uint32_t nw = level(up && x > 0 ? up[x - 1] : 0);
      const uint32_t ne = level(up && x + 1 < w ? up[x + 1] : 0);
      const uint32_t ww = level(x > 1 ? row[x - 2] : 0);
      const uint32_t nn = level(up2 ? up2[x] : 0);
      const uint32_t context = (size_class << 16) | (left << 12) | (above << 8) |
                               (static_cast<uint32_t>(step.of[nw]) << 6) | (static_cast<uint32_t>(step.of[ne]) << 4) |
                               (static_cast<uint32_t>(step.of[ww]) << 2) | step.of[nn];
      if (!fn(context, static_cast<size_t>(y) * w + x, left_index, above_index)) return false;
    }
  }
  return true;
}

// The context of the colour within its level.
inline uint32_t WithinContext(uint32_t size_class, const Levels& lv, int level, uint32_t left_index, uint32_t above_index) {
  auto place = [&](uint32_t index) -> uint32_t {
    return lv.of[index] == level ? index - lv.first[static_cast<size_t>(level)] : 4;
  };
  return kLevelContexts + ((size_class - 4) * 16 + static_cast<uint32_t>(level)) * 25 + place(left_index) * 5 + place(above_index);
}

}  // namespace

ContextTileModel::ContextTileModel() : index_(kContexts, -1) {}

void ContextTileModel::Reset() {
  std::fill(index_.begin(), index_.end(), -1);
  stats_.clear();
}

ContextTileModel::Stats& ContextTileModel::At(uint32_t context) {
  int32_t& i = index_[context];
  if (i < 0) {
    i = static_cast<int32_t>(stats_.size());
    Stats s;
    std::fill(std::begin(s.freq), std::end(s.freq), uint16_t{1});
    s.total = kSymbols;
    stats_.push_back(s);
  }
  return stats_[static_cast<size_t>(i)];
}

void ContextTileModel::Update(Stats& s, int symbol) {
  s.freq[symbol] = static_cast<uint16_t>(s.freq[symbol] + kIncrement);
  s.total = static_cast<uint16_t>(s.total + kIncrement);
  if (s.total > kLimit) {
    uint32_t total = 0;
    for (auto& f : s.freq) {
      f = static_cast<uint16_t>((f + 1) >> 1);
      total += f;
    }
    s.total = static_cast<uint16_t>(total);
  }
}

void ContextTileModel::Encode(std::span<const PaletteTile> tiles, std::vector<uint8_t>& out) {
  RangeEncoder rc(out);
  auto put = [&](Stats& s, int v, int n) {
    uint32_t cum = 0, total = 0;
    for (int k = 0; k < n; ++k) {
      if (k == v) cum = total;
      total += s.freq[k];
    }
    rc.Encode(cum, s.freq[v], total);
    Update(s, v);
  };
  for (const PaletteTile& t : tiles) {
    const int n = static_cast<int>(t.colors.size());
    if (n <= kContextTileBasicColors) {
      ForEachPixel(t.rect, SizeClass(n), Steps(n), t.indices.data(), [](uint32_t i) { return i; },
                   [&](uint32_t context, size_t at, uint32_t, uint32_t) {
                     put(At(context), t.indices[at], n);
                     return true;
                   });
      continue;
    }
    const Levels lv(n);
    const uint32_t size_class = n <= 32 ? 4 : 5;
    ForEachPixel(t.rect, size_class, Steps(kSymbols), t.indices.data(), [&](uint32_t i) -> uint32_t { return lv.of[i]; },
                 [&](uint32_t context, size_t at, uint32_t left, uint32_t above) {
                   const int v = t.indices[at];
                   const int level = lv.of[static_cast<size_t>(v)];
                   put(At(context), level, kSymbols);
                   if (lv.size(level) > 1) {
                     put(At(WithinContext(size_class, lv, level, left, above)), v - lv.first[static_cast<size_t>(level)],
                         lv.size(level));
                   }
                   return true;
                 });
  }
  rc.Finish();
}

bool ContextTileModel::Decode(std::span<const uint8_t> in, std::span<PaletteTile> tiles) {
  RangeDecoder rc(in);
  auto get = [&](Stats& s, int n) {
    uint32_t total = 0;
    for (int k = 0; k < n; ++k) total += s.freq[k];
    const uint32_t target = rc.Peek(total);
    uint32_t cum = 0;
    int v = 0;
    while (v + 1 < n && cum + s.freq[v] <= target) cum += s.freq[v++];
    rc.Take(cum, s.freq[v]);
    Update(s, v);
    return v;
  };
  for (PaletteTile& t : tiles) {
    const int n = static_cast<int>(t.colors.size());
    if (n < 2 || n > kContextTileMaxColors || t.rect.w <= 0 || t.rect.h <= 0) return false;
    t.indices.assign(static_cast<size_t>(t.rect.w) * t.rect.h, 0);
    uint8_t* idx = t.indices.data();
    bool ok;
    if (n <= kContextTileBasicColors) {
      ok = ForEachPixel(t.rect, SizeClass(n), Steps(n), idx, [](uint32_t i) { return i; },
                        [&](uint32_t context, size_t at, uint32_t, uint32_t) {
                          idx[at] = static_cast<uint8_t>(get(At(context), n));
                          return !rc.overrun();
                        });
    } else {
      const Levels lv(n);
      const uint32_t size_class = n <= 32 ? 4 : 5;
      ok = ForEachPixel(t.rect, size_class, Steps(kSymbols), idx, [&](uint32_t i) -> uint32_t { return lv.of[i]; },
                        [&](uint32_t context, size_t at, uint32_t left, uint32_t above) {
                          const int level = get(At(context), kSymbols);
                          int v = lv.first[static_cast<size_t>(level)];
                          if (lv.size(level) > 1) {
                            v += get(At(WithinContext(size_class, lv, level, left, above)), lv.size(level));
                          }
                          // (A level without colours cannot come from an encoder.)
                          if (v >= n) return false;
                          idx[at] = static_cast<uint8_t>(v);
                          return !rc.overrun();
                        });
    }
    if (!ok) return false;
  }
  return true;
}

void OrderForContextModel(PaletteTile& t) {
  const size_t n = t.colors.size();
  if (n < 3) return;  // one or two colours: the first is the background already
  auto dist = [&](uint32_t c) {
    const uint32_t b = t.colors[0];
    const int d0 = static_cast<int>(c & 255) - static_cast<int>(b & 255);
    const int d1 = static_cast<int>((c >> 8) & 255) - static_cast<int>((b >> 8) & 255);
    const int d2 = static_cast<int>((c >> 16) & 255) - static_cast<int>((b >> 16) & 255);
    return d0 * d0 + d1 * d1 + d2 * d2;
  };
  std::vector<uint8_t> order(n);
  for (size_t k = 0; k < n; ++k) order[k] = static_cast<uint8_t>(k);
  std::stable_sort(order.begin() + 1, order.end(),
                   [&](uint8_t a, uint8_t b) { return dist(t.colors[a]) < dist(t.colors[b]); });
  std::vector<uint8_t> rank(n);
  std::vector<uint32_t> colors(n);
  for (size_t k = 0; k < n; ++k) {
    rank[order[k]] = static_cast<uint8_t>(k);
    colors[k] = t.colors[order[k]];
  }
  t.colors = std::move(colors);
  for (auto& i : t.indices) i = rank[i];
}

}  // namespace rdacore::codec
