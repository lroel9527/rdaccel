#pragma once

// Host side of a connection: every captured frame goes through tile change
// detection; frames with no changed tile are skipped entirely. Changed tiles
// with few colours (text, UI) go to the lossless layer; the rest are encoded
// as one HEVC picture and sent with the list of regions the viewer should
// take from it.
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
// Areas that behave like video (tile/video_region.h) get their own HEVC
// stream, cropped to the area, with its own bitrate and quality floor. Their
// tiles skip the classification above entirely; the full-screen encoder
// keeps seeing the area as it was when the area opened (so it spends nothing
// there), and when the area goes quiet its tiles are sent once more through
// the normal path, so a paused video ends up sharp.

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
#include "tile/motion.h"
#include "tile/tile_cache.h"
#include "tile/tile_diff.h"
#include "tile/video_region.h"
#include "transport/transport.h"

namespace rdacore::session {

using HostStats = rdacore::HostStats;

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
  //   * Video (motion, photos, video areas, refinement and the still-screen
  //     polish) is coded as without text mode, at the same bitrate: the
  //     mode saves on text, not by making what moves look worse.
  // See SetTextMode() to switch while streaming.
  bool text_mode = false;
  int text_colors = 16;
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
  // be sent losslessly or still refining. A source that stops delivering
  // frames while the screen is static (ScreenCaptureKit) would leave them at
  // video quality, or stale, until the next change; the Host re-submits the
  // last frame instead. Not thread-safe against OnFrame.
  bool work_pending() const {
    return force_keyframe_ || !carry_video_tiles_.empty() || pending_lossless_count_ > 0 || refining_ > 0 ||
           polish_owed_ || coarse_count_ > 0;
  }

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
  void OnReceive(transport::Channel ch, std::span<const uint8_t> bytes);
  // Tile indices covered by `r` (tile aligned).
  std::vector<size_t> TilesOf(const Rect& r) const;
  // Encodes the video areas that need it into `out`; false = a region
  // encoder failed (the areas are then dropped and a keyframe is scheduled).
  bool EncodeRegions(const capture::CapturedFrame& frame, const std::vector<bool>& dirty,
                     uint64_t frame_id, std::vector<uint8_t>& out, size_t& bytes, size_t& pictures,
                     uint64_t& us);
  void FinishRegions(const std::vector<uint8_t>& busy, const std::vector<uint8_t>& changed);
  // The encoder's picture is not the screen: lossless tiles are one plain
  // colour in it, and tiles the viewer got by a copy or from its cache are
  // whatever was there before. Colour is coded at half resolution, so the
  // outermost pixels of a rect the viewer takes from the picture took on
  // some of the colour next to them: a line of the wrong tint along the
  // edge of a wallpaper or an icon, 28 levels off where the tile beside it
  // still held a frame of a full-screen transition, and there for good (the
  // tile counted as polished). Before a picture is coded, the pixels within
  // kVideoSurround of each rect taken from it (`taken`) that are not taken
  // themselves are given the screen's content. Not in video areas, which
  // the full-screen picture does not follow.
  static constexpr int32_t kVideoSurround = 2;
  void SurroundVideoRects(const capture::CapturedFrame& frame, const std::vector<tile::TileChange>& taken);
  void Send(transport::Channel ch, const proto::Message& m);

  transport::ITransport* transport_;
  std::unique_ptr<codec::IHevcEncoder> encoder_;
  int32_t width_, height_;
  HostOptions options_;
  tile::TileDiffer differ_;
  // What the video encoder is fed: the captured frame with lossless tiles
  // flattened (see the comment at the top).
  std::vector<uint8_t> enc_input_;
  codec::LosslessEncoder lossless_;
  // The previous frame as captured, for move detection.
  std::vector<uint8_t> prev_;
  std::vector<uint8_t> scratch_;
  // Per tile: video frames left in which the viewer still takes this tile.
  std::vector<uint8_t> refine_;
  // Video tiles of a picture the encoder dropped, listed with the next one.
  std::vector<tile::TileChange> carry_video_tiles_;
  size_t refining_ = 0;  // tiles with refine_ > 0
  // Still-screen polish (HostOptions::still_refine_frames). Per tile: 0 =
  // nothing owed (lossless, polished, or not in the encoder's picture);
  // kUnpolished = its content changed since it was last polished; otherwise
  // the still pictures it is yet to be taken from.
  static constexpr uint8_t kUnpolished = 255;
  std::vector<uint8_t> polish_;
  bool polish_owed_ = false;  // some tile is waiting for, or in, its polish

  // Text mode. Per tile, what the viewer shows is not what the host had in
  // the previous frame: kStaleText = part of it waits for the lossless
  // budget (it is in pending_lossless_ too); kStaleVideo = a video picture
  // that carries it was held back or dropped. Such a tile is coded whole
  // when it changes again (nothing in it may be assumed unchanged), is not
  // stored in the tile cache, and what is copied out of it is owed as well.
  static constexpr uint8_t kStaleText = 1;
  static constexpr uint8_t kStaleVideo = 2;
  // ... and what it shows there is a keyframe's placeholder (the tile's
  // background colour), not older content. Nothing is copied out of such a
  // tile: that painted blocks of plain colour over tiles that were fine.
  // Out of a tile that shows older content copies do go (a window being
  // dragged over it moves on as it is, a little behind, and is repaired
  // when its tiles' turn comes): without them one tile that had to wait
  // made every tile the window touched wait, each coded whole in every
  // frame, which no budget covers.
  static constexpr uint8_t kStalePlaceholder = 4;
  std::vector<uint8_t> stale_;
  // Text may use all of the bitrate: the allowance is on video, and what
  // the path cannot take is held back by the rate control's queue limit.
  // (At three quarters a fast scroll left rows of older text standing.)
  static constexpr double kTextLosslessShare = 1.0;
  // With a screenful owed (more than kTextCoarseBacklog tiles: the first
  // picture, a new window, a page that scrolled in), tiles the viewer has
  // nothing useful for first go out at kTextCoarseColors: a third of the
  // bytes again, so a page is readable in a third of the time. Such a tile
  // counts as rough (coarse_ remembers that it had its quick pass) and is
  // sent again at text_colors once the rest is done and the screen is
  // nearly still.
  // Only when the backlog would take longer than kTextCoarseBacklogSeconds
  // at the current bitrate: on a path that takes the screen in a moment the
  // quick pass would only add its bytes to the bill.
  static constexpr double kTextCoarseBacklogSeconds = 1.5;
  static constexpr size_t kTextCoarseBacklog = 48;
  static constexpr int kTextCoarseColors = 4;
  std::vector<uint8_t> coarse_;
  size_t coarse_count_ = 0;
  // Tiles deferred when they changed (not a keyframe's): they go before the
  // rest of what is owed, newest first, so that what the user is typing
  // does not wait behind a screenful that is still turning sharp.
  std::vector<size_t> urgent_;
  // Deflated size of lossless tiles over their raw (palette + index) size,
  // a running estimate: what a tile is counted at against the budget.
  double lossless_ratio_ = 0.25;

  // Tile cache bookkeeping (see OnFrame).
  static constexpr uint64_t kStableFrames = 8;
  static constexpr uint64_t kHotHoldUs = 250'000;  // a hot tile stays hot over gaps this short
  static constexpr size_t kMaxSeen = 1 << 16;
  static constexpr size_t kMaxRecurringStores = 16;
  uint64_t frame_no_ = 0;
  std::vector<uint64_t> born_;  // per tile: frame_no_ when its content appeared
  std::vector<uint64_t> hot_us_;  // per tile: capture time it last changed as a hot tile
  std::unordered_map<uint64_t, uint8_t> seen_;  // content hash -> times it appeared
  tile::TileCacheIndex cache_{proto::kTileCacheSlots};

  // What the viewer shows for each tile, compared with the host's pixels.
  // Rough tiles that stay static are refreshed (see HostOptions::refresh).
  static constexpr uint8_t kRough = 0;    // first-pass video, or copied from it
  static constexpr uint8_t kRefined = 1;  // video after its refinement frames
  static constexpr uint8_t kExact = 2;    // lossless
  static constexpr uint64_t kRefreshDelay = 6;      // frames static before a refresh
  static constexpr size_t kMaxRefreshPerFrame = 24;  // spreads the cost out
  std::vector<uint8_t> quality_;
  std::vector<uint8_t> slot_quality_ = std::vector<uint8_t>(proto::kTileCacheSlots, kRough);

  // The host's own copy of each slot's pixels, and per tile position the slot
  // that last stored what was there. When a dragged window uncovers part of
  // a tile, the uncovered pixels usually equal that stored content, so they
  // can be painted from the slot instead of being coded.
  std::vector<uint8_t> slot_pixels_;  // kTileCacheSlots tiles of kTileSize^2 BGRA
  std::vector<int64_t> tile_slot_;     // per tile, -1 = none
  std::vector<uint64_t> tile_slot_hash_;
  // Video areas and their encoders (by stream id).
  tile::VideoRegionTracker regions_;
  struct RegionEncoder {
    std::unique_ptr<codec::IHevcEncoder> enc;
    bool need_key = true;
  };
  std::unordered_map<uint8_t, RegionEncoder> region_enc_;
  int64_t RegionBitrate(const Rect& r) const;
  bool regions_disabled_ = false;      // a region encoder could not be created
  std::vector<size_t> pending_sharpen_;  // tiles of closed areas, for the next frame
  // Progressive keyframes: tiles still owed their lossless version.
  std::vector<uint8_t> pending_lossless_;
  size_t pending_lossless_count_ = 0;
  size_t pending_cursor_ = 0;              // where the next frame continues
  double lossless_bytes_per_tile_ = 1500;  // running estimate, for the budget
  uint64_t progressive_last_us_ = 0;       // capture time of the last frame that sent some
  // Lossless budget (see OnFrame). New content deflates to about half its
  // raw palette + index size; content seen before costs much less (the
  // stream's history), but that cannot be told in advance.
  uint64_t last_frame_us_ = 0;                        // capture time of the previous frame
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
  double lossless_tokens_ = 0;  // bytes, may go negative
  double lossless_cap_ = 0;     // what the bucket holds at most, bytes
  bool lossless_tokens_set_ = false;
  bool progressive_active_ = false;        // a progressive keyframe is being completed
  bool sharpen_video_ = false;             // ... and was: re-send the video tiles sharp
  // Tiles sent losslessly by a progressive keyframe whose text is still in
  // the encoder's input (flattened in one go, at the end).
  std::vector<uint8_t> unflattened_;
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
  bool holding_back_ = false;        // the previous frame's view
  uint64_t lossless_tokens_us_ = 0;  // when the bucket was last filled
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
