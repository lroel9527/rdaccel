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

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "capture/capturer.h"
#include "rdaccel/host.h"
#include "codec/lossless.h"
#include "codec/video_codec.h"
#include "proto/messages.h"
#include "tile/motion.h"
#include "tile/tile_cache.h"
#include "tile/tile_diff.h"
#include "tile/video_region.h"
#include "transport/transport.h"

namespace rdaccel::session {

using HostStats = rdaccel::HostStats;

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
  bool tile_cache = true;
  // Re-send tiles left at first-pass video quality once they are static
  // (typically content that scrolled in or was dragged).
  bool refresh = false;

  // Video region streams. Off unless an encoder factory is given.
  using EncoderFactory = std::function<std::unique_ptr<codec::IHevcEncoder>(
      const codec::EncoderConfig&, std::string* error)>;
  EncoderFactory region_encoder_factory = nullptr;
  int32_t fps = 60;
  int64_t bitrate_bps = 8'000'000;          // the whole session's budget
  int32_t region_min_qp = 27;               // quality floor inside video areas
  int64_t region_bitrate_bps = 0;           // 0 = a share of bitrate_bps by area
  tile::VideoRegionParams region_params = {};
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

  // Makes the next encoded frame a keyframe.
  void RequestKeyframe() { force_keyframe_ = true; }

  HostStats stats() const;

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
  size_t refining_ = 0;  // tiles with refine_ > 0

  // Tile cache bookkeeping (see OnFrame).
  static constexpr uint64_t kStableFrames = 8;
  static constexpr size_t kMaxSeen = 1 << 16;
  static constexpr size_t kMaxRecurringStores = 16;
  uint64_t frame_no_ = 0;
  std::vector<uint64_t> born_;  // per tile: frame_no_ when its content appeared
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

}  // namespace rdaccel::session
