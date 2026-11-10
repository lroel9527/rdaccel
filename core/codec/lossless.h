#pragma once

// Lossless layer for tiles with few colours (text, UI chrome, icons).
//
// A tile qualifies when it has at most `max_colors` distinct RGB values. It is
// coded as a palette (sorted by frequency, so the background is usually index
// 0 in every tile) plus one index byte per pixel. All tiles of a frame go
// through one deflate stream that persists across frames, so a glyph or a
// toolbar that was sent before is mostly a back-reference the next time.
// The stream is reset on keyframes so a viewer can start from any keyframe.
//
// The same stream carries the frame's other region commands (tile cache
// stores and loads, moves): they are small and repetitive, so deflate makes
// them nearly free, where separate messages cost 10-15 bytes each.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rdacore/geometry.h"
#include "rdacore/image.h"

namespace rdacore::codec {

struct PaletteTile {
  Rect rect;
  std::vector<uint32_t> colors;  // 0x00RRGGBB, most frequent first
  std::vector<uint8_t> indices;  // rect.w * rect.h, row-major
};

// Fills `out` and returns true if `r` has at most `max_colors` (<= 256)
// distinct colours. Alpha is ignored.
bool ExtractPalette(const ImageView& img, const Rect& r, int max_colors, PaletteTile& out);

// Like ExtractPalette, but a tile with more colours is reduced to at most
// `max_colors` of its own colours (each pixel takes the nearest one): the
// shades of anti-aliasing merge, edges stay where they are, and the tile
// costs a fraction of the exact one (a page of text at 16 colours deflates
// to about 40% of its exact size). The most frequent colour, usually the
// background, is always kept as it is.
//
// A tile with up to `exact_colors` colours always qualifies (it is text or
// UI). One with more, up to `max_source_colors`, qualifies only when the
// reduction stays close to the picture (text on a soft gradient or a
// translucent panel does; a photo does not and returns false).
// `reduced`, if given, tells whether colours were merged.
bool ExtractReducedPalette(const ImageView& img, const Rect& r, int max_colors, int exact_colors,
                           int max_source_colors, PaletteTile& out, bool* reduced = nullptr);

// Writes the tile into a BGRA buffer (alpha = 255). The caller guarantees the
// rect lies inside the buffer.
void PaintPalette(const PaletteTile& t, uint8_t* bgra, int32_t stride);

// Tile cache command: store what the viewer shows at `rect` into `slot`, or
// paint the part of `slot` starting at (sx, sy) at `rect`.
struct SlotOp {
  uint32_t slot = 0;
  Rect rect;
  uint32_t sx = 0;
  uint32_t sy = 0;
  friend bool operator==(const SlotOp&, const SlotOp&) = default;
};

// Copy what the viewer shows at `dst` shifted by (-dx, -dy) to `dst`.
struct MoveOp {
  Rect dst;
  int32_t dx = 0;
  int32_t dy = 0;
  friend bool operator==(const MoveOp&, const MoveOp&) = default;
};

// Everything one frame changes besides the video picture. The viewer applies
// it as: stores, moves, loads, (video), tiles.
struct RegionOps {
  std::vector<SlotOp> stores;
  std::vector<MoveOp> moves;
  std::vector<SlotOp> loads;
  std::vector<PaletteTile> tiles;
  // Encoder only: tiles of 2..16 colours whose indices go through the
  // context model (codec/context_tiles.h) instead of deflate. A decoder
  // returns them in `tiles`, after the others.
  std::vector<PaletteTile> context_tiles;
  bool empty() const {
    return stores.empty() && moves.empty() && loads.empty() && tiles.empty() && context_tiles.empty();
  }
};

class LosslessEncoder {
 public:
  LosslessEncoder();
  ~LosslessEncoder();
  LosslessEncoder(const LosslessEncoder&) = delete;
  LosslessEncoder& operator=(const LosslessEncoder&) = delete;

  // Compresses one frame's region commands. With `reset` the deflate history
  // is dropped first; the matching decoder call must then also reset.
  // The indices of ops.context_tiles are appended to `*context` (required
  // if there are any; their rects and palettes go into the deflate stream,
  // after everything else, so a stream without them is as it always was).
  std::vector<uint8_t> Encode(const RegionOps& ops, bool reset, std::vector<uint8_t>* context = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class LosslessDecoder {
 public:
  LosslessDecoder();
  ~LosslessDecoder();
  LosslessDecoder(const LosslessDecoder&) = delete;
  LosslessDecoder& operator=(const LosslessDecoder&) = delete;

  // Returns false on corrupt input (the decoder then needs a reset frame).
  // Rects outside a `width` x `height` screen, slots >= `slots` and cache
  // parts outside a kTileSize tile are rejected.
  // `context`: the block Encode() wrote for the frame's context tiles.
  bool Decode(std::span<const uint8_t> data, bool reset, int32_t width, int32_t height,
              uint32_t slots, RegionOps& ops, std::string* error = nullptr,
              std::span<const uint8_t> context = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdacore::codec
