#pragma once

// The picture the full-screen video encoder is fed.
//
// It is not the screen. Lossless rects are one plain colour in it (the
// viewer paints them over the picture, and the encoder spends nothing on
// text it would blur anyway); tiles the viewer got by a copy or from its
// tile cache keep whatever was there before (unchanged pixels cost the
// encoder nothing, moved ones would make it code the motion); a video area
// keeps what it showed when the area opened (the area has a stream of its
// own). Only the rects the viewer takes from a picture must match the
// screen, and their surroundings must not tint them: colour is coded at
// half resolution and the deblocking filter reaches across edges.

#include <cstdint>
#include <vector>

#include "codec/lossless.h"
#include "rdacore/geometry.h"
#include "rdacore/image.h"
#include "tile/tile_diff.h"
#include "tile/video_region.h"

namespace rdacore::session {

class EncoderPicture {
 public:
  EncoderPicture(int32_t width, int32_t height)
      : width_(width), height_(height), px_(static_cast<size_t>(width) * height * 4, 0) {}

  ImageView view() const { return {px_.data(), width_, height_, width_ * 4}; }

  // The screen's pixels of `r`.
  void Copy(const ImageView& screen, const Rect& r);
  // `r` all one colour (0xRRGGBB).
  void Fill(const Rect& r, uint32_t rgb);
  // The colour at (x, y) (0xRRGGBB).
  uint32_t ColourAt(int32_t x, int32_t y) const;
  // Whether `r` holds the same pixels as `other` (a full screen of the same
  // size, rows of width * 4 bytes).
  bool Matches(const std::vector<uint8_t>& other, const Rect& r) const;

  // The outermost pixels of a rect the viewer takes from the picture took
  // on some of the colour next to them: a line of the wrong tint along the
  // edge of a wallpaper or an icon, 28 levels off where the tile beside it
  // still held a frame of a full-screen transition, and there for good (the
  // tile counted as polished). So the pixels within kVideoSurround of each
  // rect taken (`taken`) that are not taken themselves get the screen's
  // content. Not in video areas, which the full-screen picture does not
  // follow.
  static constexpr int32_t kVideoSurround = 2;
  void SurroundTaken(const ImageView& screen, const std::vector<tile::TileChange>& taken,
                     const tile::VideoRegionTracker& areas, int32_t cols, int32_t rows);
  // Text mode flattens each lossless rect (the viewer paints it over the
  // picture), and the pixels the viewer takes from the picture next to such
  // a rect take on some of its colour: a tint along the last row of a
  // wallpaper or icon tile beside a text tile, kept for good as text mode
  // codes such tiles once. SurroundTaken does not reach these (it skips
  // keyframes and the inside of the rects taken). So the pixels of
  // `lossless` within kVideoSurround of a pixel taken from the picture get
  // the screen's content.
  void KeepScreenNextToVideo(const ImageView& screen, const std::vector<tile::TileChange>& taken,
                             const std::vector<codec::PaletteTile>& lossless);

 private:
  int32_t width_, height_;
  std::vector<uint8_t> px_;      // BGRA, rows of width_ * 4 bytes
  std::vector<uint8_t> marks_;   // KeepScreenNextToVideo: the pixels taken from the picture (reused)
};

}  // namespace rdacore::session
