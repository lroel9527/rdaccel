#pragma once

// Index maps of tiles with at most 16 colours (text mode), coded with an
// adaptive context model and a range coder instead of deflate.
//
// Deflate finds repeated byte strings. Anti-aliased text has few: glyphs sit
// at fractional positions, so the same letter is a different pattern of
// greys each time. What does repeat is the local structure: next to
// background there is mostly background, next to a dark pixel something a
// little lighter. So each pixel is coded from the pixels left of and above
// it, with statistics that are learnt as the stream goes and shared by all
// tiles (they persist across frames, like the deflate history). On pages of
// text rendered by macOS this comes to about half of what deflate needs
// (16 colours: 152 -> 79 kB a 1080p screen; 4 colours: 79 -> 43 kB).
//
// For the statistics to mean the same in every tile, a tile's colours are
// ordered by their distance from its first (most frequent) colour, the
// background: index 0 is background, the highest index the colour furthest
// from it, typically the text. The encoder does the ordering; the decoder
// takes the palette as it comes.
//
// Context of a pixel: the palette size (2, 3-4, 5-8, 9-16), the indices of
// its left and upper neighbours, and those of four more (upper left, upper
// right, two to the left, two above) in four steps each. Pixels outside the
// tile count as background.

#include <cstdint>
#include <span>
#include <vector>

#include "codec/lossless.h"

namespace rdacore::codec {

inline constexpr int kContextTileMaxColors = 16;

// The statistics; one per direction of a stream, reset when the stream is.
class ContextTileModel {
 public:
  ContextTileModel();
  void Reset();

  // Appends the indices of `tiles` (each with 2..16 colours, in the order
  // described above) to `out` as one range-coded block.
  void Encode(std::span<const PaletteTile> tiles, std::vector<uint8_t>& out);
  // Fills the indices of `tiles`, whose rects and colours are set. False on
  // input that cannot be a block of these tiles (never reads past `in`).
  bool Decode(std::span<const uint8_t> in, std::span<PaletteTile> tiles);

 private:
  struct Stats {
    uint16_t freq[kContextTileMaxColors];
    uint16_t total;  // of all sixteen
  };
  Stats& At(uint32_t context);
  static void Update(Stats& s, int symbol);

  std::vector<int32_t> index_;  // context -> position in stats_, -1 = not seen yet
  std::vector<Stats> stats_;
};

// Orders `t`'s colours as the model expects (see above) and renumbers its
// indices to match.
void OrderForContextModel(PaletteTile& t);

}  // namespace rdacore::codec
