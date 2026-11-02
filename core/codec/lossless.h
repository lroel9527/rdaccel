#pragma once

// Lossless layer for tiles with few colours (text, UI chrome, icons).
//
// A tile qualifies when it has at most `max_colors` distinct RGB values. It is
// coded as a palette (sorted by frequency, so the background is usually index
// 0 in every tile) plus one index byte per pixel. All tiles of a frame go
// through one deflate stream that persists across frames, so a glyph or a
// toolbar that was sent before is mostly a back-reference the next time.
// The stream is reset on keyframes so a viewer can start from any keyframe.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rdaccel/geometry.h"
#include "rdaccel/image.h"

namespace rdaccel::codec {

struct PaletteTile {
  Rect rect;
  std::vector<uint32_t> colors;  // 0x00RRGGBB, most frequent first
  std::vector<uint8_t> indices;  // rect.w * rect.h, row-major
};

// Fills `out` and returns true if `r` has at most `max_colors` (<= 256)
// distinct colours. Alpha is ignored.
bool ExtractPalette(const ImageView& img, const Rect& r, int max_colors, PaletteTile& out);

// Writes the tile into a BGRA buffer (alpha = 255). The caller guarantees the
// rect lies inside the buffer.
void PaintPalette(const PaletteTile& t, uint8_t* bgra, int32_t stride);

class LosslessEncoder {
 public:
  LosslessEncoder();
  ~LosslessEncoder();
  LosslessEncoder(const LosslessEncoder&) = delete;
  LosslessEncoder& operator=(const LosslessEncoder&) = delete;

  // Compresses one frame's tiles. With `reset` the deflate history is dropped
  // first; the matching decoder call must then also reset.
  std::vector<uint8_t> Encode(std::span<const PaletteTile> tiles, bool reset);

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
  // Tiles outside a `width` x `height` screen are rejected.
  bool Decode(std::span<const uint8_t> data, bool reset, int32_t width, int32_t height,
              std::vector<PaletteTile>& tiles, std::string* error = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdaccel::codec
