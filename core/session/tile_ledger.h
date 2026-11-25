#pragma once

// The host's account of what the viewer shows, tile by tile and pixel by
// pixel, and of the work each tile still has coming.
//
// HostSession decides everything the viewer shows, so it knows without
// asking what the viewer has for every tile: the host's pixels exactly
// (lossless), video that is finished, video that is to be sent again, older
// content that waits. That is each tile's state (TileState). Alongside it
// each tile has a plan: to be sent again (owed), to be taken from the next
// pictures (refinement, the still-screen polish). The host changes a tile
// only through the transitions below, each named for what happened to it,
// and asks the ledger which tiles have work due (the Due* queries).
//
// What the viewer has is also kept for every pixel, because copies move
// pixels across tiles: a scroll is one copy of most of the screen, a dragged
// window copies itself along, the tile cache paints back what it stored.
// Going by tiles, a scroll was as good as the worst tile it read from: one
// icon at video quality made every tile it touched rough, and in text mode
// each was owed again once the scroll stopped (about a quarter of a slow
// scroll's traffic). The pixel qualities travel with the pixels instead, and
// a tile that received copies is judged by the pixels it ended up with.
//
// The rule for every state: never better than what the viewer shows. A
// tile or pixel marked worse than it is costs a re-send; one marked better
// stays as it is for good, as nothing would look at it again.

#include <cstdint>
#include <span>
#include <vector>

#include "rdacore/geometry.h"
#include "tile/motion.h"

namespace rdacore::session {

// How the viewer has a tile, against the host's pixels.
inline constexpr uint8_t kRough = 0;    // first-pass video, or copied from it: to be sent again
inline constexpr uint8_t kRefined = 1;  // video that had its refinement: finished
inline constexpr uint8_t kExact = 2;    // lossless (or a copy of that)

// How the viewer has a pixel. Ordered from best to worst: a rect is as good
// as its worst pixel (WorstPixel).
inline constexpr uint8_t kPxExact = 0;     // lossless, or a copy of that
inline constexpr uint8_t kPxPolished = 1;  // video that is finished (polished, or text mode's picture
                                           // measured close to the screen)
// Text mode: video measured far from the screen (HostOptions::
// text_video_min_db), to be taken again from a later picture once the
// screen is nearly still (the retake plan). It used to count as finished
// whatever the picture was like, and a starved picture's blocks stayed.
inline constexpr uint8_t kPxPoor = 2;
inline constexpr uint8_t kPxVideo = 3;     // any other video: may be text that went as video in
                                           // motion or for want of budget, or a photo not yet refined

// The cells video is measured and taken again by (16 x 16, as text mode
// decides tiles that do not fit a palette).
inline constexpr int32_t kCellSize = 16;

// Text mode: why a tile shows something other than what the host had in the
// previous frame (TileState::stale, bits).
inline constexpr uint8_t kStaleText = 1;   // part of it waits for the lossless budget (it is owed)
inline constexpr uint8_t kStaleVideo = 2;  // a video picture that carries it was held back or dropped
// ... and what it shows there is a keyframe's placeholder (the tile's
// background colour), not older content. Nothing is copied out of such a
// tile: that painted blocks of plain colour over tiles that were fine.
inline constexpr uint8_t kStalePlaceholder = 4;

// TileState::polish: content that changed since it was last polished.
inline constexpr uint8_t kUnpolished = 255;

// Frames a tile stands still before work that waits for that is due (it
// may still be changing until then).
inline constexpr uint64_t kRefreshDelay = 6;

struct TileState {
  // --- What the viewer shows. ---
  uint8_t quality = kRough;
  // Text mode, bits: what the viewer shows is not what the host had in the
  // previous frame. Such a tile is coded whole when it changes again
  // (nothing in it may be assumed unchanged), is not stored in the tile
  // cache, and what is copied out of it is owed as well.
  uint8_t stale = 0;
  // It shows the quick pass (a screenful owed on a slow path: tiles first at
  // a few colours); counts as rough and is sent again at text_colors.
  bool coarse = false;
  // Progressive keyframe: sent losslessly while its text is still in the
  // encoder's picture (flattened in one go at the end).
  bool unflattened = false;

  // --- Its plan. ---
  // To be sent again: losslessly where a palette takes it, else as video
  // (the owed queue: a progressive keyframe's tiles, text that went as video
  // in motion or over the budget, rough tiles once the screen is still).
  bool owed = false;
  // Pictures the viewer still takes it from: the encoder refines static
  // content over a few pictures (HostOptions::refine_frames).
  uint8_t refine = 0;
  // Still-screen polish: 0 = nothing owed (lossless, polished, or not in the
  // encoder's picture); kUnpolished = changed since it was last polished;
  // otherwise the still pictures it is yet to be taken from.
  uint8_t polish = kUnpolished;
  // Palette content that went as video because it kept changing ends up
  // refined and polished by the encoder, never rough: such a tile is looked
  // at once it stands still and owed losslessly if a palette takes it. born
  // + 1 of the content that was looked at (0 = none), so each is tried once.
  uint64_t palette_checked = 0;

  // --- Its content. ---
  uint64_t born = 0;    // the frame number when its content appeared
  uint64_t born_us = 0;  // ... and its capture time
  uint64_t hot_us = 0;  // capture time it last changed as a hot tile (0 = not hot)
  // The tile cache slot that last stored what was here, and the content it
  // stored: when a dragged window uncovers part of the tile, the uncovered
  // pixels usually equal it and can be painted from the slot.
  int64_t slot = -1;
  uint64_t slot_hash = 0;
};

class TileLedger {
 public:
  // A screen of `cols` x `rows` tiles and `width` x `height` pixels: every
  // tile rough with nothing planned, every pixel video.
  void Resize(int32_t width, int32_t height, int32_t cols, int32_t rows);
  size_t size() const { return tiles_.size(); }
  int32_t cols() const { return cols_; }

  TileState& operator[](size_t idx) { return tiles_[idx]; }
  const TileState& operator[](size_t idx) const { return tiles_[idx]; }

  // --- Transitions of the whole screen. ---

  // A keyframe codes every tile again: nothing is owed, stale, a quick pass
  // or unflattened any more.
  void StartKeyframe();
  // The encoder failed: the viewer will get a keyframe, and until then
  // nothing it shows may count as more than rough.
  void ForgetAll();

  // --- Transitions of a tile. ---

  // Into the owed queue (no-op if it is there).
  void Owe(size_t idx);
  // Out of it: coded now, or no longer worth it (became part of a video area).
  void Settle(size_t idx);
  // The pictures the viewer takes the tile from (0 = none). Coded as video
  // again, its still-screen polish is owed afresh.
  void SetRefine(size_t idx, uint8_t pictures);
  // The viewer took the tile from one more picture of its refinement: true
  // when that was the last one (the encoder has had its chance to sharpen
  // it).
  bool Refined(size_t idx);
  // Content arrived in a video area: the area's stream shows it.
  void EnterArea(size_t idx, uint64_t frame_no, uint64_t now_us);

  // --- Plans. ---

  // Work that is due by the tile's own state (the caller adds what depends
  // on the rest of the frame: video areas, tiles that change now):
  // Rough and static: to be sent again (HostOptions::refresh).
  bool DueRefresh(size_t idx, uint64_t frame_no) const {
    const TileState& t = tiles_[idx];
    return t.quality == kRough && t.refine == 0 && frame_no - t.born >= kRefreshDelay;
  }
  // ... and not owed yet: into the owed queue.
  bool DueOwing(size_t idx, uint64_t frame_no) const { return DueRefresh(idx, frame_no) && !tiles_[idx].owed; }
  // Finished video that may be palette content (see palette_checked).
  bool DuePaletteCheck(size_t idx, uint64_t frame_no) const {
    const TileState& t = tiles_[idx];
    return t.quality == kRefined && t.refine == 0 && !t.owed && t.stale == 0 && !t.coarse &&
           frame_no - t.born >= kRefreshDelay && t.palette_checked != t.born + 1;
  }
  // Rough with nothing planned yet: frames must keep coming until it is
  // owed, or it stays as it is until the screen changes again.
  bool RoughWaiting(size_t idx) const {
    const TileState& t = tiles_[idx];
    return t.quality == kRough && t.refine == 0 && !t.owed;
  }

  size_t owed_count() const { return owed_count_; }
  size_t refining() const { return refining_; }
  size_t coarse_count() const;
  // Owed tiles go out round the screen from here, so that each gets its
  // turn.
  size_t cursor = 0;

  // --- Poor video and its retakes (text mode). ---

  // New content in the tile: its cells' retakes count from 0 again.
  void NewContent(size_t idx);
  // How often the video of the cell holding pixel (x, y) was taken again
  // since its tile's content changed.
  uint8_t& retakes(int32_t x, int32_t y) {
    return retakes_[static_cast<size_t>(y / kCellSize) * cell_cols_ + x / kCellSize];
  }
  // What the cell holding pixel (x, y) measured when its video was last
  // taken from a picture (PSNR in dB, 0 = not measured since it changed).
  uint8_t& measured(int32_t x, int32_t y) {
    return measured_[static_cast<size_t>(y / kCellSize) * cell_cols_ + x / kCellSize];
  }
  // Whether some pixel may be kPxPoor (set when one may have become so,
  // cleared by the scan that found none). Work is pending meanwhile.
  bool poor_possible() const { return poor_possible_; }
  void NotePoor() { poor_possible_ = true; }
  void ClearPoorPossible() { poor_possible_ = false; }
  // Poor video is no longer followed (the pictures cannot be measured): it
  // counts as finished, as before the measuring.
  void GiveUpPoor();
  struct Mix {
    bool exact = false;  // some pixel of it is exact
    bool poor = false;   // some pixel of it is poor
  };
  Mix MixOf(const Rect& r) const;
  bool HasPoor(const Rect& r) const;
  // The pixels of `r` of quality `from` become `to`.
  void Replace(const Rect& r, uint8_t from, uint8_t to);

  // --- Pixels. ---

  void SetPixels(const Rect& r, uint8_t q);
  // A row of the pixels' qualities (width bytes), for measuring.
  uint8_t* PixelRow(int32_t y) { return px_.data() + static_cast<size_t>(y) * width_; }
  // Exact pixels of `r` become kPxVideo: the viewer takes `r` from a picture.
  void TakenFromVideo(const Rect& r);
  uint8_t WorstPixel(const Rect& r) const;
  // The pixels' quality goes where the pixels go.
  void ApplyMoves(std::span<const tile::Move> moves);
  // The qualities of `r` (inside `tile`), as a kTileSize x kTileSize block
  // `out` keeps for a cache slot, and back.
  void SavePixels(const Rect& tile, uint8_t* out) const;
  void RestorePixels(const Rect& tile, const Rect& r, const uint8_t* saved);

 private:
  int32_t width_ = 0, height_ = 0, cols_ = 0, cell_cols_ = 0;
  std::vector<TileState> tiles_;
  std::vector<uint8_t> retakes_;   // per 16 x 16 cell
  std::vector<uint8_t> measured_;  // per 16 x 16 cell
  bool poor_possible_ = false;
  size_t owed_count_ = 0;
  size_t refining_ = 0;
  std::vector<uint8_t> px_;  // width_ * height_
  std::vector<uint8_t> px_scratch_;
};

}  // namespace rdacore::session
