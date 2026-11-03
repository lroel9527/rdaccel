#pragma once

// Finds areas of the screen that behave like video: many neighbouring tiles
// that need the video encoder in most frames (a playing movie, a game, a 3D
// view). The host gives each such area its own HEVC stream (see
// session/host_session.h), cropped to the area and with its own bitrate and
// quality, so the full-screen encoder is left with the occasional photo or
// gradient and the area does not pay for, or disturb, the rest of the screen.
//
// Purely tile based, so it is cheap and deterministic:
//   * every processed frame, each tile reports whether it was "busy" (sent
//     through video, or changed inside an existing area);
//   * a tile is active when it was busy in at least `min_busy` of the last
//     `window` frames;
//   * 8-connected groups of active tiles become candidate areas (bounding
//     box, at least `min_tiles_w` x `min_tiles_h` tiles and dense enough);
//   * candidates become areas after they are seen for `confirm` frames in a
//     row, areas grow (as a new area) when a candidate sticks out of them,
//     and close once nothing in them changed for `linger` frames.

#include <cstdint>
#include <vector>

#include "rdaccel/geometry.h"

namespace rdaccel::tile {

struct VideoRegionParams {
  int32_t window = 30;    // frames of history per tile (at most 64)
  int32_t min_busy = 20;  // busy frames in the window to be active
  int32_t min_tiles_w = 2;
  int32_t min_tiles_h = 2;
  double min_density = 0.5;  // active tiles / bounding-box tiles
  int32_t confirm = 3;       // frames a candidate must persist
  int32_t linger = 30;       // idle frames before an area closes
  int32_t max_regions = 2;
  int32_t min_resize_gap = 60;  // frames between two growths of one area
};

struct VideoRegion {
  uint8_t stream = 0;  // 1..255, a fresh id whenever the rect changes
  Rect rect;           // pixels, tile aligned (clipped at the screen edge)
  uint64_t created = 0;
  uint64_t last_change = 0;
};

class VideoRegionTracker {
 public:
  explicit VideoRegionTracker(VideoRegionParams p = {}) : p_(p) {}

  // Starts over for a screen of `cols` x `rows` tiles of a `width` x `height`
  // picture.
  void Reset(int32_t width, int32_t height);

  // Index of the area containing tile (col, row), or -1.
  int RegionOf(int32_t col, int32_t row) const;

  // Records one processed frame. `busy` has one entry per tile (row-major);
  // `changed` likewise says which tiles changed at all (to keep areas open).
  // Returns the areas that closed in this call (their tiles should get a
  // final sharp picture from the main path). Areas that were replaced by a
  // larger one are not reported as closed.
  std::vector<VideoRegion> Update(const std::vector<uint8_t>& busy,
                                  const std::vector<uint8_t>& changed);

  // Drops every area without reporting it (keyframes, encoder failures).
  void Clear();

  // Closes one area now (e.g. its encoder failed) and returns it.
  VideoRegion Close(int index);

  const std::vector<VideoRegion>& regions() const { return regions_; }
  int32_t cols() const { return cols_; }
  int32_t rows() const { return rows_; }

 private:
  struct Candidate {
    Rect tiles;  // in tile units
    int32_t seen = 0;
  };
  Rect ToPixels(const Rect& tiles) const;
  uint8_t NextStream();

  VideoRegionParams p_;
  int32_t width_ = 0, height_ = 0, cols_ = 0, rows_ = 0;
  uint64_t frame_ = 0;
  uint8_t next_stream_ = 1;
  std::vector<uint64_t> history_;  // per tile, bit 0 = latest frame
  std::vector<Candidate> candidates_;
  std::vector<VideoRegion> regions_;
};

}  // namespace rdaccel::tile
