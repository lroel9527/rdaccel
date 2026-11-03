#include "codec/lossless.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "proto/wire.h"

namespace rdaccel::codec {
namespace {

// Text tiles are small, so a stronger level costs little time and finds more
// of the repeats in the history.
constexpr int kDeflateLevel = 6;
constexpr uint32_t kEmpty = 0xffffffffu;  // never a real colour: alpha is masked off

inline uint32_t Rgb(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v & 0x00ffffffu;  // little-endian BGRA -> 0x00RRGGBB
}

// Bits per index for a palette size. Packing is not just about size: with one
// byte per index, a 3-colour tile has only 27 distinct 3-byte strings, so the
// deflate hash chains get so long that matches from earlier frames are never
// reached. Packed bytes look far more varied.
int IndexBits(size_t colors) {
  if (colors <= 2) return 1;
  if (colors <= 4) return 2;
  if (colors <= 16) return 4;
  return 8;
}

// Rows start on a byte boundary so identical rows pack to identical bytes.
size_t PackedSize(const Rect& r, int bits) {
  return static_cast<size_t>((r.w * bits + 7) / 8) * r.h;
}

void PackIndices(const PaletteTile& t, int bits, proto::ByteWriter& w) {
  const uint8_t* idx = t.indices.data();
  for (int32_t y = 0; y < t.rect.h; ++y) {
    uint32_t acc = 0;
    int n = 0;
    for (int32_t x = 0; x < t.rect.w; ++x) {
      acc = (acc << bits) | *idx++;
      n += bits;
      if (n == 8) {
        w.u8(static_cast<uint8_t>(acc));
        acc = 0;
        n = 0;
      }
    }
    if (n > 0) w.u8(static_cast<uint8_t>(acc << (8 - n)));
  }
}

bool UnpackIndices(std::span<const uint8_t> in, int bits, size_t max_index, PaletteTile& t) {
  t.indices.resize(static_cast<size_t>(t.rect.w) * t.rect.h);
  const uint32_t mask = (1u << bits) - 1;
  const uint8_t* p = in.data();
  uint8_t* out = t.indices.data();
  for (int32_t y = 0; y < t.rect.h; ++y) {
    int n = 0;
    uint8_t cur = 0;
    for (int32_t x = 0; x < t.rect.w; ++x) {
      if (n == 0) {
        cur = *p++;
        n = 8;
      }
      n -= bits;
      const uint8_t v = static_cast<uint8_t>((cur >> n) & mask);
      if (v > max_index) return false;
      *out++ = v;
    }
  }
  return true;
}

}  // namespace

bool ExtractPalette(const ImageView& img, const Rect& r, int max_colors, PaletteTile& out) {
  max_colors = std::clamp(max_colors, 1, 256);
  // Open-addressing table, 4x the colour budget so probes stay short.
  constexpr int kSlots = 1024;
  std::array<uint32_t, kSlots> keys;
  std::array<uint32_t, kSlots> counts;
  std::array<uint8_t, kSlots> first_index;
  keys.fill(kEmpty);
  std::vector<uint32_t> colors;
  colors.reserve(static_cast<size_t>(max_colors));
  out.rect = r;
  out.indices.resize(static_cast<size_t>(r.w) * r.h);

  uint32_t last = kEmpty;
  int last_slot = 0;
  size_t i = 0;
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    const uint8_t* p = img.data + static_cast<size_t>(y) * img.stride + static_cast<size_t>(r.x) * 4;
    for (int32_t x = 0; x < r.w; ++x, p += 4, ++i) {
      const uint32_t c = Rgb(p);
      int slot = last_slot;
      if (c != last) {
        slot = static_cast<int>((c * 0x9e3779b1u) >> 22);  // top 10 bits
        while (keys[slot] != kEmpty && keys[slot] != c) slot = (slot + 1) & (kSlots - 1);
        if (keys[slot] == kEmpty) {
          if (static_cast<int>(colors.size()) == max_colors) return false;
          keys[slot] = c;
          counts[slot] = 0;
          first_index[slot] = static_cast<uint8_t>(colors.size());
          colors.push_back(c);
        }
        last = c;
        last_slot = slot;
      }
      ++counts[slot];
      out.indices[i] = first_index[slot];
    }
  }

  // Sort by frequency (ties by value) so the same content gets the same
  // indices in every tile, which is what lets deflate match across tiles.
  std::vector<uint8_t> order(colors.size());
  for (size_t k = 0; k < order.size(); ++k) order[k] = static_cast<uint8_t>(k);
  auto count_of = [&](uint8_t k) {
    int slot = static_cast<int>((colors[k] * 0x9e3779b1u) >> 22);
    while (keys[slot] != colors[k]) slot = (slot + 1) & (kSlots - 1);
    return counts[slot];
  };
  std::vector<uint32_t> freq(colors.size());
  for (size_t k = 0; k < colors.size(); ++k) freq[k] = count_of(static_cast<uint8_t>(k));
  std::sort(order.begin(), order.end(), [&](uint8_t a, uint8_t b) {
    return freq[a] != freq[b] ? freq[a] > freq[b] : colors[a] < colors[b];
  });
  std::array<uint8_t, 256> remap{};
  out.colors.resize(colors.size());
  for (size_t k = 0; k < order.size(); ++k) {
    remap[order[k]] = static_cast<uint8_t>(k);
    out.colors[k] = colors[order[k]];
  }
  for (auto& idx : out.indices) idx = remap[idx];
  return true;
}

void PaintPalette(const PaletteTile& t, uint8_t* bgra, int32_t stride) {
  std::array<uint32_t, 256> px{};
  for (size_t k = 0; k < t.colors.size(); ++k) px[k] = t.colors[k] | 0xff000000u;
  size_t i = 0;
  for (int32_t y = 0; y < t.rect.h; ++y) {
    uint8_t* row = bgra + static_cast<size_t>(t.rect.y + y) * stride + static_cast<size_t>(t.rect.x) * 4;
    for (int32_t x = 0; x < t.rect.w; ++x, ++i) std::memcpy(row + x * 4, &px[t.indices[i]], 4);
  }
}

// ---------------------------------------------------------------------------

struct LosslessEncoder::Impl {
  z_stream zs{};
};

LosslessEncoder::LosslessEncoder() : impl_(std::make_unique<Impl>()) {
  deflateInit(&impl_->zs, kDeflateLevel);
}

LosslessEncoder::~LosslessEncoder() { deflateEnd(&impl_->zs); }

namespace {

// Rects are written relative to the previous one, which keeps the numbers
// small and repetitive for deflate.
void PutRectDelta(proto::ByteWriter& w, const Rect& r, Rect& prev) {
  w.svarint(r.x - prev.x);
  w.svarint(r.y - prev.y);
  w.varint(static_cast<uint64_t>(r.w));
  w.varint(static_cast<uint64_t>(r.h));
  prev = r;
}

bool GetRectDelta(proto::ByteReader& r, Rect& out, Rect& prev, int32_t width, int32_t height) {
  int32_t dx = 0, dy = 0;
  uint32_t w = 0, h = 0;
  if (!(r.svarint32(dx) && r.svarint32(dy) && r.varint32(w) && r.varint32(h))) return false;
  const int64_t x = static_cast<int64_t>(prev.x) + dx, y = static_cast<int64_t>(prev.y) + dy;
  if (x < 0 || y < 0 || w == 0 || h == 0 || x + w > width || y + h > height) {
    return false;
  }
  out = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(w),
         static_cast<int32_t>(h)};
  prev = out;
  return true;
}

}  // namespace

std::vector<uint8_t> LosslessEncoder::Encode(const RegionOps& ops, bool reset) {
  if (reset) deflateReset(&impl_->zs);
  proto::ByteWriter w;
  Rect prev;
  w.varint(ops.stores.size());
  for (const auto& st : ops.stores) {
    w.varint(st.slot);
    PutRectDelta(w, st.rect, prev);
  }
  w.varint(ops.moves.size());
  for (const auto& m : ops.moves) {
    PutRectDelta(w, m.dst, prev);
    w.svarint(m.dx);
    w.svarint(m.dy);
  }
  w.varint(ops.loads.size());
  for (const auto& ld : ops.loads) {
    w.varint(ld.slot);
    PutRectDelta(w, ld.rect, prev);
    w.varint(ld.sx);
    w.varint(ld.sy);
  }
  w.varint(ops.tiles.size());
  for (const auto& t : ops.tiles) {
    PutRectDelta(w, t.rect, prev);
    w.u8(static_cast<uint8_t>(t.colors.size() - 1));
    for (uint32_t c : t.colors) {
      w.u8(static_cast<uint8_t>(c));
      w.u8(static_cast<uint8_t>(c >> 8));
      w.u8(static_cast<uint8_t>(c >> 16));
    }
    if (t.colors.size() > 1) PackIndices(t, IndexBits(t.colors.size()), w);
  }
  const auto& raw = w.data();

  z_stream& zs = impl_->zs;
  std::vector<uint8_t> out(deflateBound(&zs, raw.size()) + 16);
  zs.next_in = const_cast<Bytef*>(raw.data());
  zs.avail_in = static_cast<uInt>(raw.size());
  size_t produced = 0;
  for (;;) {
    zs.next_out = out.data() + produced;
    zs.avail_out = static_cast<uInt>(out.size() - produced);
    deflate(&zs, Z_SYNC_FLUSH);
    produced = out.size() - zs.avail_out;
    if (zs.avail_out != 0) break;
    out.resize(out.size() * 2);
  }
  out.resize(produced);
  return out;
}

// ---------------------------------------------------------------------------

struct LosslessDecoder::Impl {
  z_stream zs{};
  bool broken = false;
};

LosslessDecoder::LosslessDecoder() : impl_(std::make_unique<Impl>()) { inflateInit(&impl_->zs); }

LosslessDecoder::~LosslessDecoder() { inflateEnd(&impl_->zs); }

bool LosslessDecoder::Decode(std::span<const uint8_t> data, bool reset, int32_t width,
                             int32_t height, uint32_t slots, RegionOps& ops, std::string* error) {
  auto fail = [&](const char* why) {
    impl_->broken = true;
    if (error) *error = why;
    return false;
  };
  z_stream& zs = impl_->zs;
  if (reset) {
    inflateReset(&zs);
    impl_->broken = false;
  }
  if (impl_->broken) return fail("lossless stream needs a reset after an earlier error");

  // A frame never holds more than every pixel once plus small headers; this
  // bounds memory against a hostile stream.
  const size_t limit = static_cast<size_t>(width) * height * 2 + 4096;
  std::vector<uint8_t> raw(std::min<size_t>(limit, std::max<size_t>(data.size() * 8, 4096)));
  zs.next_in = const_cast<Bytef*>(data.data());
  zs.avail_in = static_cast<uInt>(data.size());
  size_t produced = 0;
  for (;;) {
    zs.next_out = raw.data() + produced;
    zs.avail_out = static_cast<uInt>(raw.size() - produced);
    const int st = inflate(&zs, Z_SYNC_FLUSH);
    produced = raw.size() - zs.avail_out;
    if (st != Z_OK && st != Z_BUF_ERROR) return fail("corrupt lossless data");
    if (zs.avail_in == 0 && zs.avail_out != 0) break;
    if (raw.size() >= limit) return fail("lossless frame too large");
    raw.resize(std::min(limit, raw.size() * 2));
  }

  proto::ByteReader r(std::span<const uint8_t>(raw.data(), produced));
  const uint64_t max_count = static_cast<uint64_t>(width) * height;
  Rect prev;
  uint64_t n = 0;
  auto get_slot_op = [&](SlotOp& op, bool with_offset) {
    if (!(r.varint32(op.slot) && op.slot < slots && GetRectDelta(r, op.rect, prev, width, height) &&
          op.rect.w <= kTileSize && op.rect.h <= kTileSize)) {
      return false;
    }
    if (!with_offset) return true;
    return r.varint32(op.sx) && r.varint32(op.sy) && op.sx + op.rect.w <= static_cast<uint32_t>(kTileSize) &&
           op.sy + op.rect.h <= static_cast<uint32_t>(kTileSize);
  };
  if (!r.varint(n) || n > max_count) return fail("bad store count");
  ops.stores.resize(static_cast<size_t>(n));
  for (auto& st : ops.stores) {
    if (!get_slot_op(st, false)) return fail("bad cache store");
  }
  if (!r.varint(n) || n > max_count) return fail("bad move count");
  ops.moves.resize(static_cast<size_t>(n));
  for (auto& m : ops.moves) {
    if (!(GetRectDelta(r, m.dst, prev, width, height) && r.svarint32(m.dx) && r.svarint32(m.dy))) {
      return fail("bad move");
    }
    const int64_t sx = static_cast<int64_t>(m.dst.x) - m.dx, sy = static_cast<int64_t>(m.dst.y) - m.dy;
    if (sx < 0 || sy < 0 || sx + m.dst.w > width || sy + m.dst.h > height) {
      return fail("move source outside the screen");
    }
  }
  if (!r.varint(n) || n > max_count) return fail("bad load count");
  ops.loads.resize(static_cast<size_t>(n));
  for (auto& ld : ops.loads) {
    if (!get_slot_op(ld, true)) return fail("bad cache load");
  }
  if (!r.varint(n) || n > max_count) return fail("bad tile count");
  auto& tiles = ops.tiles;
  tiles.resize(static_cast<size_t>(n));
  for (auto& t : tiles) {
    uint8_t nc = 0;
    if (!(GetRectDelta(r, t.rect, prev, width, height) && r.u8(nc))) {
      return fail("bad tile header");
    }
    t.colors.resize(static_cast<size_t>(nc) + 1);
    for (auto& c : t.colors) {
      uint8_t b = 0, g = 0, rr = 0;
      if (!(r.u8(b) && r.u8(g) && r.u8(rr))) return fail("truncated palette");
      c = b | (g << 8) | (static_cast<uint32_t>(rr) << 16);
    }
    const size_t npx = static_cast<size_t>(t.rect.w) * t.rect.h;
    if (t.colors.size() == 1) {
      t.indices.assign(npx, 0);
      continue;
    }
    const int bits = IndexBits(t.colors.size());
    std::span<const uint8_t> packed;
    if (!r.bytes(PackedSize(t.rect, bits), packed)) return fail("truncated indices");
    if (!UnpackIndices(packed, bits, nc, t)) return fail("palette index out of range");
  }
  if (!r.at_end()) return fail("trailing lossless data");
  return true;
}

}  // namespace rdaccel::codec
