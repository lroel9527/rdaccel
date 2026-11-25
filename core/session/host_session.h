#pragma once

// Host side of a connection: every captured frame goes through tile change
// detection; frames with no changed tile are skipped entirely. Changed tiles
// with few colours (text, UI) go to the lossless layer; the rest are encoded
// as one video picture (HEVC, or H.264 where chosen) and sent with the list
// of regions the viewer should take from it.
//
// Before that, when many tiles change, they are checked for scrolling or
// window moves (tile/motion.h); moved tiles become copy commands. Tiles whose
// new content the viewer has kept in its tile cache (tile/tile_cache.h) are
// painted from there.
//
// The video encoder always sees a full frame, but lossless tiles are replaced
// by their dominant colour in that frame. Typing therefore costs the video
// encoder nothing (often no video frame at all), and the encoder never spends
// bits on text it would blur anyway.
//
// Areas that behave like video (tile/video_region.h) get their own video
// stream (of the session's codec), cropped to the area, with its own bitrate
// and quality floor. Their tiles skip the classification above entirely; the full-screen encoder
// keeps seeing the area as it was when the area opened (so it spends nothing
// there), and when the area goes quiet its tiles are sent once more through
// the normal path, so a paused video ends up sharp.
//
// What the viewer shows of every tile and pixel, and the work each tile
// still has coming, is the TileLedger's (session/tile_ledger.h); the picture
// the encoder is fed is the EncoderPicture's (session/encoder_picture.h).
// OnFrame takes each frame through a fixed series of stages (see there).

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "capture/capturer.h"
#include "rdacore/host.h"
#include "codec/lossless.h"
#include "codec/video_codec.h"
#include "proto/messages.h"
#include "session/encoder_picture.h"
#include "session/lossless_bucket.h"
#include "session/slot_book.h"
#include "session/tile_ledger.h"
#include "tile/motion.h"
#include "tile/tile_cache.h"
#include "tile/tile_diff.h"
#include "tile/video_region.h"
#include "transport/transport.h"

namespace rdacore::session {

using HostStats = rdacore::HostStats;

// What one frame did: why its rects went to the video encoder, what the
// encoder's picture was coded for and how large it came out. See
// HostOptions::trace.
struct FrameTrace {
  uint64_t frame_no = 0;
  bool key = false;      // a keyframe
  bool still = false;    // coded with the still-screen QP floor
  bool sharpen = false;  // the picture that ends a progressive keyframe
  size_t picture_bytes = 0;  // the full-screen encoder's picture; 0 = none
  int video_rects = 0;       // rects of new content the viewer takes from the picture
  // ... of which, by why they are not lossless:
  int video_colours = 0;         // too many colours for a palette
  int video_hot = 0;             // few enough for a palette, too many for content that changes every frame
  int video_budget = 0;          // a palette, but the lossless budget of the frame was spent
  int video_keyframe_first = 0;  // a palette, sent as video first with a progressive keyframe
  int video_motion = 0;          // text mode: part of motion, too many colours for a hot rect
  int refine_tiles = 0;  // tiles taken again from the picture: ordinary refinement
  int polish_tiles = 0;  // ... the still-screen polish
  int lossless_rects = 0;
  int hot = 0;          // tiles that also changed in the frame before
  int hot_unmoved = 0;  // ... of which no move accounts for (text mode)
  int moved = 0;        // copy commands
  bool mass_motion = false;  // text mode treated the frame as motion
};

struct HostOptions {
  bool lossless = true;
  int lossless_max_colors = 256;
  int lossless_max_colors_hot = 4;  // see HostConfig
  bool detect_moves = true;
  // After a tile was video coded, the viewer keeps taking it from the next
  // this many video frames, so it gets the encoder's refinements of static
  // content (the low-latency encoder starts coarse and sharpens over a few
  // frames). Encoding continues while tiles are refining. 0 disables.
  int refine_frames = 8;
  // Once the screen is nearly still, tiles left at video quality (icons,
  // photos, gradients: whatever has too many colours for the lossless layer)
  // are taken from this many more pictures, coded with the QP floor lowered
  // to `still_min_qp`. The floor that suits a moving picture leaves a still
  // one visibly soft, and the encoder needs about a second to use a finer
  // one: on a 2816 x 1762 desktop 8 pictures at the usual floor left 64 of
  // 1232 tiles more than 30 levels off, 60 pictures at 12 none. Nothing is
  // spent while the picture moves. 0 frames (or 0 QP, or refine_frames 0:
  // no refinement at all) disables.
  int still_refine_frames = 60;
  int still_min_qp = 12;
  bool tile_cache = true;
  // Re-send tiles left at first-pass video quality once they are static
  // (typically content that scrolled in or was dragged).
  bool refresh = false;
  // Progressive keyframes: a keyframe (the first picture, or a refresh the
  // viewer asked for) goes out as video only, which is several times smaller
  // than the lossless tiles and shows up quickly on a slow link. The tiles
  // that qualify for the lossless layer then follow over the next frames,
  // within about half the bitrate budget.
  bool progressive = true;

  // Text mode: as little traffic as possible with sharp text, whether the
  // link is metered or slow.
  //   * Tiles are reduced to `text_colors` of their own colours before the
  //     lossless layer (codec::ExtractReducedPalette; more where that many
  //     would show): edges stay where they are, the shades of anti-aliasing
  //     merge. Text on a soft gradient or a translucent panel (more than
  //     lossless_max_colors colours) qualifies too, where it would otherwise
  //     be video coded and blurred.
  //   * Text that stands still, or changes in a few tiles (typing), goes
  //     out exactly and at once: a keyframe carries the whole screen's text
  //     if half a second of the bitrate covers it.
  //   * Motion (many tiles changing frame after frame: a scroll that
  //     re-renders, a zoom to full screen, a film) goes through the video
  //     encoder as a whole, as without text mode, and what is text is sent
  //     again exactly once it stops. So does text beyond the lossless
  //     budget. Tiles wait as the viewer has them (stale) only with a
  //     keyframe on a slow path.
  //   * Video (motion, photos, video areas) is coded as without text mode,
  //     at the same bitrate, but it is not refined or polished afterwards
  //     (text_refine): what is not text gets the one picture.
  // See SetTextMode() to switch while streaming.
  bool text_mode = false;
  int text_colors = 16;
  // Text mode decides by 16 x 16 cells where a tile as a whole has too many
  // colours for a palette (text beside a window's shadow, an icon, a
  // translucent bar: a fifth of the text cells of a desktop sat in such
  // tiles and stayed at video quality). A cell of text (tile/text_cell.h)
  // goes out losslessly, in reduced colours where that stays faithful and
  // as it is otherwise; any other cell that fits a palette of up to
  // `split_cells` colours does too; the rest of the tile goes to the
  // encoder. 0 = off: the whole tile goes to the encoder.
  int split_cells = 16;
  // ... also tiles that changed in the frame before (typing next to a
  // picture, a caret). Off: those go to the encoder as before and are split
  // once they stop changing (splitting them at once cost a tenth more
  // traffic with a candidate window open, for text that changes again).
  bool split_hot = false;
  // Text mode's first aim is little traffic with sharp text; what is not
  // text does not have to look as in the normal mode. So what the encoder
  // codes in text mode is not refined (refine_frames) or polished
  // (still_refine_frames): on a real desktop those pictures were 59% of the
  // traffic while typing and 61% of an idle screen's. true = as in the
  // normal mode.
  bool text_refine = false;
  // Text mode closes a video area this long after it stopped changing as an
  // area (without text mode: region_params.linger_us, 250 ms): after a
  // full-screen transition, or when a film is paused, the text in the area
  // stays at the area's quality until it closes and is coded again. With
  // the Host re-submitting a still screen every 100 ms the area closes on
  // the first re-submit instead of the third, and its text is sharp about
  // 200 ms after the motion instead of 450. 0 = as without text mode.
  int64_t text_linger_us = 100'000;
  // Text mode's tiles (2..16 colours) go through the context model
  // (codec/context_tiles.h), about half the bytes of deflate on text. Only
  // for a viewer that decodes it (proto::kCapContextTiles); see
  // SetContextTiles().
  bool context_tiles = false;
  // ... and also its tiles of 17..64 colours (proto::kCapContextTiles64).
  bool context_tiles_64 = false;

  // Video region streams. Off unless an encoder factory is given.
  using EncoderFactory = std::function<std::unique_ptr<codec::IHevcEncoder>(
      const codec::EncoderConfig&, std::string* error)>;
  EncoderFactory region_encoder_factory = nullptr;
  int32_t fps = 60;
  int64_t bitrate_bps = 8'000'000;          // the whole session's budget
  int32_t region_min_qp = 27;               // quality floor inside video areas
  int64_t region_bitrate_bps = 0;           // 0 = a share of bitrate_bps by area
  tile::VideoRegionParams region_params = {};
  // Called once for every frame that is not skipped, with what the frame
  // did (see FrameTrace). For tools that account for the traffic
  // (rdaccel_clip); costs one more palette search for each rect that goes
  // to the encoder.
  std::function<void(const FrameTrace&)> trace = nullptr;
  // Install the transport's receive callback (input, keyframe requests).
  // Host turns this off: its Link receives and calls in.
  bool own_receiver = true;
  // Send the Hello on the video channel instead of control: after a
  // resolution change it must arrive in order with the frames.
  bool hello_on_video = false;
  // Frame ids continue across sessions of one host.
  uint64_t first_frame_id = 1;
};

class HostSession {
 public:
  using InputFn = std::function<void(const proto::InputEvent&)>;

  HostSession(transport::ITransport* transport, std::unique_ptr<codec::IHevcEncoder> encoder,
              int32_t width, int32_t height, HostOptions options = {});

  // Called with input events from the viewer (on the transport thread).
  void SetInputHandler(InputFn fn);

  // Feed captured frames here (typically the capturer's callback). Returns
  // false if the encoder failed on this frame; `error` explains.
  bool OnFrame(const capture::CapturedFrame& frame, std::string* error = nullptr);

  // Changes the video bitrate budget (adaptive bitrate): the full-screen
  // encoder gets `bps`, each video area its share of it.
  void SetBitrate(int64_t bps);

  // Turns text mode (HostOptions::text_mode) on or off between frames. Off
  // makes the next frame a keyframe: the reduced colours are replaced by the
  // exact ones. Not thread-safe against OnFrame.
  void SetTextMode(bool on);
  bool text_mode() const { return options_.text_mode; }
  // Whether the viewer decodes context tiles (HostOptions::context_tiles).
  // May change between any two frames.
  void SetContextTiles(bool on, bool up_to_64 = false) {
    options_.context_tiles = on;
    options_.context_tiles_64 = on && up_to_64;
  }

  // Makes the next encoded frame a keyframe.
  void RequestKeyframe() { force_keyframe_ = true; }
  // Sends the stream's Hello again (thread-safe), for a viewer that
  // connected after the stream started.
  void SendHello();
  bool keyframe_pending() const { return force_keyframe_; }
  // Something is still owed to the viewer that needs no new picture: a
  // keyframe, video tiles of a picture the encoder dropped, tiles waiting to
  // be sent losslessly or still refining, a video area that is open (it
  // closes, and its picture is sharpened, only on a frame). A source that
  // stops delivering frames while the screen is static (ScreenCaptureKit)
  // would leave them at video quality, or stale, until the next change (a
  // paused video stayed at the area's quality); the Host re-submits the last
  // frame instead. Not thread-safe against OnFrame.
  bool work_pending() const {
    return force_keyframe_ || !carry_video_tiles_.empty() || ledger_.owed_count() > 0 || ledger_.refining() > 0 ||
           polish_owed_ || coarse_count_ > 0 || rough_waiting_ || !pending_sharpen_.empty() || !regions_.regions().empty();
  }

  // How the viewer has `r` as of the last frame: 0 = all of it from the
  // lossless layer (or a copy of that), 1 = some of it video that had its
  // still-screen polish, 2 = some of it other video. For tools that measure
  // (rdaccel_clip). Not thread-safe against OnFrame.
  int ShownQuality(const Rect& r) const { return ledger_.size() == 0 ? 2 : ledger_.WorstPixel(r); }

  // The screen as of the last frame passed to OnFrame (BGRA, rows of
  // width * 4 bytes), e.g. to encode a requested keyframe when no new frame
  // comes. Not thread-safe against OnFrame.
  const std::vector<uint8_t>& last_frame() const { return prev_; }
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }

  HostStats stats() const;
  // Continues the counters of an earlier session (resolution change).
  void AdoptStats(const HostStats& s);
  uint64_t next_frame_id() const { return next_frame_id_; }  // under the frame path

 private:
  // What one frame works with on its way through OnFrame's stages (defined
  // in host_session.cpp).
  struct Frame;

  // OnFrame's stages, in order (see OnFrame).
  void BeginFrame(Frame& f);
  void TrackAreas(Frame& f);
  void FillLosslessBucket(Frame& f);
  void PlanOwedWork(Frame& f);
  void PlanPolish(Frame& f);
  bool NothingToDo(const Frame& f) const;
  void StoreLeavingTiles(Frame& f);
  void ClassifyChanges(Frame& f);
  void DetectMotion(Frame& f);
  void CodeTiles(Frame& f);
  void AssemblePicture(Frame& f);
  bool EncodePicture(Frame& f, std::string* error, bool* done);
  void SendFrame(Frame& f);

  // CodeTiles, for one changed (or owed) tile, by how it can be sent.
  void CodeChange(Frame& f, size_t i);
  void CodeMovedTile(Frame& f, size_t i, bool was_stale);
  bool LoadFromCache(Frame& f, size_t i);
  bool CodeFromSlot(Frame& f, size_t i, bool was_stale);
  bool CodeSplitTile(Frame& f, size_t i);
  void CodeWholeTile(Frame& f, size_t i);
  // Codes `r` (all or part of tile `c`) losslessly if it has few colours,
  // else through the video encoder (see the definition). CodeRect also keeps
  // the pixels' quality up to date with what `r` is now.
  enum : int { kCodedLossless = 0, kCodedVideo = 1, kDeferred = 2 };
  int CodeRectAs(Frame& f, const tile::TileChange& c, const Rect& r, bool is_hot);
  int CodeRect(Frame& f, const tile::TileChange& c, const Rect& r, bool is_hot);
  // Paints the parts of `r` (inside tile `c`) that equal what the tile's
  // last stored slot holds from that slot; returns the parts still to code.
  std::vector<Rect> PaintFromSlot(Frame& f, const tile::TileChange& c, const Rect& r, double min_share, uint8_t& q);
  // AssemblePicture's parts.
  void ListCarriedTiles(Frame& f);
  void CoarsenVideoParts(Frame& f);
  void ContinueRefinement(Frame& f);
  void FinishVideoTiles(Frame& f);
  void SharpenAfterProgressive(Frame& f);
  void PolishStillScreen(Frame& f);

  void OnReceive(transport::Channel ch, std::span<const uint8_t> bytes);
  size_t TileIndex(const tile::TileChange& c) const { return static_cast<size_t>(c.row) * differ_.cols() + c.col; }
  Rect TileRect(size_t idx) const;
  // Tile indices covered by `r` (tile aligned).
  std::vector<size_t> TilesOf(const Rect& r) const;
  bool InArea(size_t idx) const;
  // A copied rect is as good as the worst tile it was copied from.
  uint8_t QualityUnder(const Rect& r) const;
  // Encodes the video areas that need it into `out`; false = a region
  // encoder failed (the areas are then dropped and a keyframe is scheduled).
  bool EncodeRegions(const capture::CapturedFrame& frame, const std::vector<bool>& dirty,
                     uint64_t frame_id, std::vector<uint8_t>& out, size_t& bytes, size_t& pictures,
                     uint64_t& us);
  void FinishRegions(const std::vector<uint8_t>& busy, const std::vector<uint8_t>& changed, uint64_t now_us);
  int64_t RegionBitrate(const Rect& r) const;
  void Send(transport::Channel ch, const proto::Message& m);

  transport::ITransport* transport_;
  std::unique_ptr<codec::IHevcEncoder> encoder_;
  int32_t width_, height_;
  HostOptions options_;
  tile::TileDiffer differ_;
  // What the viewer shows of every tile and pixel, and what each tile still
  // has coming (session/tile_ledger.h).
  TileLedger ledger_;
  // What the video encoder is fed (session/encoder_picture.h).
  EncoderPicture picture_;
  codec::LosslessEncoder lossless_;
  // The previous frame as captured (updated tile by tile as tiles are
  // coded), for move detection and what the tile cache stores.
  std::vector<uint8_t> prev_;
  // The viewer's tile cache (session/slot_book.h).
  SlotBook slots_;
  // Video tiles of a picture the encoder dropped, listed with the next one.
  std::vector<tile::TileChange> carry_video_tiles_;
  bool polish_owed_ = false;  // some tile is waiting for, or in, its polish
  // Text mode: some tile was left rough without refinement to follow (it
  // kept changing and went to the encoder whole) and will be owed once it
  // has stood still for kRefreshDelay frames. Until then frames must keep
  // coming, or it stays as it is until the screen changes again.
  bool rough_waiting_ = false;
  size_t coarse_count_ = 0;  // tiles showing the quick pass (TileState::coarse)

  // Text mode: many tiles that change frame after frame are motion (a
  // scroll that re-renders its text, a window zooming to full screen, a
  // film), not typing. Motion goes through the video encoder as a whole,
  // as without text mode: every tile of it in the same picture, a little
  // soft, and what is text is sent again exactly once it stops. Deciding
  // tile by tile (this one sharp now, that one later, a third as video)
  // showed the screen cut into pieces of different ages for as long as it
  // moved. A few such tiles (typing, a caret, a clock) stay exact.
  static constexpr size_t kTextMaxHotTiles = 8;

  // Text may use all of the bitrate: the allowance is on video, and what
  // the path cannot take is held back by the rate control's queue limit.
  // (At three quarters a fast scroll left rows of older text standing.)
  static constexpr double kTextLosslessShare = 1.0;
  // With a screenful owed (more than kTextCoarseBacklog tiles: the first
  // picture, a new window, a page that scrolled in), tiles the viewer has
  // nothing useful for first go out at kTextCoarseColors: a third of the
  // bytes again, so a page is readable in a third of the time. Such a tile
  // counts as rough (TileState::coarse remembers that it had its quick
  // pass) and is sent again at text_colors once the rest is done and the
  // screen is nearly still.
  // Only when the backlog would take longer than kTextCoarseBacklogSeconds
  // at the current bitrate: on a path that takes the screen in a moment the
  // quick pass would only add its bytes to the bill.
  static constexpr double kTextCoarseBacklogSeconds = 1.5;
  static constexpr size_t kTextCoarseBacklog = 48;
  static constexpr int kTextCoarseColors = 4;
  // Deflated size of lossless tiles over their raw (palette + index) size,
  // a running estimate: what a tile is counted at against the budget.
  double lossless_ratio_ = 0.25;

  // Tile cache bookkeeping (see StoreLeavingTiles).
  static constexpr uint64_t kStableFrames = 8;
  static constexpr uint64_t kHotHoldUs = 250'000;  // a hot tile stays hot over gaps this short
  static constexpr size_t kMaxSeen = 1 << 16;
  static constexpr size_t kMaxRecurringStores = 16;
  uint64_t frame_no_ = 0;
  std::unordered_map<uint64_t, uint8_t> seen_;  // content hash -> times it appeared

  static constexpr size_t kMaxRefreshPerFrame = 24;  // HostOptions::refresh: spreads the cost out

  // Video areas and their encoders (by stream id).
  tile::VideoRegionTracker regions_;
  struct RegionEncoder {
    std::unique_ptr<codec::IHevcEncoder> enc;
    bool need_key = true;
  };
  std::unordered_map<uint8_t, RegionEncoder> region_enc_;
  bool regions_disabled_ = false;      // a region encoder could not be created
  std::vector<size_t> pending_sharpen_;  // tiles of closed areas, for the next frame
  double lossless_bytes_per_tile_ = 1500;  // running estimate, for the owed tiles' budget
  uint64_t progressive_last_us_ = 0;       // capture time of the last frame that sent owed tiles
  // Lossless budget (see FillLosslessBucket). New content deflates to about
  // half its raw palette + index size; content seen before costs much less
  // (the stream's history), but that cannot be told in advance.
  uint64_t last_frame_us_ = 0;          // capture time of the previous frame
  std::array<uint64_t, 3> late_us_{};  // capture times of the last frames that came late
  // The budget is a bucket of bytes: it fills at kLosslessRateShare of the
  // bitrate, holds at most kLosslessBurstFrames frame intervals of the
  // bitrate, and is charged what the lossless part of each frame really
  // came to (so frames the budget had to let through, such as keyframes,
  // hold back the next ones).
  static constexpr double kLosslessBurstFrames = 3;
  static constexpr double kLosslessRateShare = 0.5;
  // A tile is counted at its raw palette + index size, which deflate
  // hardly ever exceeds, so a frame stays within the bucket; content seen
  // before costs less, and the bucket is charged only what it cost.
  static constexpr double kLosslessRawShare = 1.0;
  LosslessBucket bucket_;
  bool progressive_active_ = false;  // a progressive keyframe is being completed
  bool sharpen_video_ = false;       // ... and was: re-send the video tiles sharp
  // Share of the bitrate the owed tiles may use. The screen is usually static
  // meanwhile; changes still go first, and the transport's queue limit holds
  // latency down if the estimate is off.
  static constexpr double kProgressiveShare = 0.8;
  // ... of at most this much time, in one frame.
  static constexpr double kOwedMaxUs = 100'000;
  // Lossless share while content is moving and the link is tight (the
  // previous frame had moved tiles and sent lossless tiles as video for want
  // of budget). A fast link never runs out, so nothing changes there.
  static constexpr double kLosslessMovingShare = 0.2;
  bool moving_ = false;
  // The previous frame changed at most 2% of the tiles.
  bool quiet_ = true;
  bool holding_back_ = false;  // the previous frame's view
  static constexpr size_t kMaxProgressivePerFrame = 64;
  uint64_t next_frame_id_ = 1;
  std::atomic<bool> force_keyframe_{true};

  mutable std::mutex mu_;
  HostStats stats_;
  InputFn on_input_;
  // One per channel: on the UDP transport the channels are separate
  // streams whose chunks may interleave mid-message.
  proto::StreamParser input_parser_;
  proto::StreamParser control_parser_;
};

// Merges changed tiles into horizontal runs, one rect per run, to keep the
// region list short.
std::vector<Rect> TilesToRegions(const std::vector<tile::TileChange>& changes);

}  // namespace rdacore::session
