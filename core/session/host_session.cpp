#include "session/host_session.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <tuple>

namespace rdacore::session {

std::vector<Rect> TilesToRegions(const std::vector<tile::TileChange>& changes) {
  std::vector<tile::TileChange> sorted = changes;
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
    return a.row != b.row ? a.row < b.row : a.col < b.col;
  });
  std::vector<Rect> out;
  for (const auto& c : sorted) {
    if (!out.empty()) {
      Rect& last = out.back();
      if (last.y == c.rect.y && last.h == c.rect.h && last.right() == c.rect.x) {
        last.w += c.rect.w;
        continue;
      }
    }
    out.push_back(c.rect);
  }
  // Then vertical runs of rects with the same columns: tiles split into
  // pixel rows (the part of a tile a scroll or a dragged window uncovered)
  // otherwise list one rect per row, and a frame with many of them sent
  // several kB of region list (7 kB in one frame on the Mac at 0.4 Mbps).
  std::sort(out.begin(), out.end(), [](const Rect& a, const Rect& b) {
    return a.x != b.x ? a.x < b.x : a.w != b.w ? a.w < b.w : a.y < b.y;
  });
  std::vector<Rect> merged;
  for (const Rect& r : out) {
    if (!merged.empty()) {
      Rect& last = merged.back();
      if (last.x == r.x && last.w == r.w && last.y + last.h == r.y) {
        last.h += r.h;
        continue;
      }
    }
    merged.push_back(r);
  }
  return merged;
}

HostSession::HostSession(transport::ITransport* transport,
                         std::unique_ptr<codec::IHevcEncoder> encoder, int32_t width,
                         int32_t height, HostOptions options)
    : transport_(transport),
      encoder_(std::move(encoder)),
      width_(width),
      height_(height),
      options_(options),
      enc_input_(static_cast<size_t>(width) * height * 4, 0),
      prev_(enc_input_.size(), 0),
      regions_(options.region_params),
      next_frame_id_(options.first_frame_id) {
  if (options_.own_receiver) {
    transport_->SetReceiver(
        [this](transport::Channel ch, std::span<const uint8_t> bytes) { OnReceive(ch, bytes); });
  }
  if (options_.text_mode) SetBitrate(options_.bitrate_bps);  // the encoder's share
  SendHello();
}

void HostSession::SendHello() {
  Send(options_.hello_on_video ? transport::Channel::kVideo : transport::Channel::kControl,
       proto::Hello{proto::kProtocolVersion, static_cast<uint32_t>(width_),
                    static_cast<uint32_t>(height_), proto::VideoCodec::kHevc});
}

void HostSession::SetInputHandler(InputFn fn) {
  std::lock_guard<std::mutex> lock(mu_);
  on_input_ = std::move(fn);
}

HostStats HostSession::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

void HostSession::AdoptStats(const HostStats& s) {
  std::lock_guard<std::mutex> lock(mu_);
  stats_ = s;
}

void HostSession::Send(transport::Channel ch, const proto::Message& m) {
  const auto bytes = proto::Encode(m);
  transport_->Send(ch, bytes);
  std::lock_guard<std::mutex> lock(mu_);
  stats_.bytes_sent += bytes.size();
}

namespace {

void CopyRectInto(const tile::ImageView& src, const Rect& r, uint8_t* dst, int32_t dst_stride) {
  const size_t row = static_cast<size_t>(r.w) * 4;
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * dst_stride + static_cast<size_t>(r.x) * 4,
                src.data + static_cast<size_t>(y) * src.stride + static_cast<size_t>(r.x) * 4, row);
  }
}

void FillRect(const Rect& r, uint32_t rgb, uint8_t* dst, int32_t dst_stride) {
  const uint32_t px = rgb | 0xff000000u;
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    uint8_t* p = dst + static_cast<size_t>(y) * dst_stride + static_cast<size_t>(r.x) * 4;
    for (int32_t x = 0; x < r.w; ++x) std::memcpy(p + x * 4, &px, 4);
  }
}

}  // namespace

bool HostSession::OnFrame(const capture::CapturedFrame& frame, std::string* error) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.frames_captured;
  }
  const bool key = force_keyframe_.exchange(false);
  // A keyframe classifies every tile, so the encoder input is rebuilt fully.
  if (key) differ_.Reset();
  auto changes = differ_.Diff(frame.bgra, key ? std::span<const Rect>() : frame.dirty);
  const size_t n_tiles = static_cast<size_t>(differ_.cols()) * differ_.rows();
  if (key) carry_video_tiles_.clear();  // the whole screen goes anyway
  if (refine_.size() != n_tiles) {
    carry_video_tiles_.clear();
    refine_.assign(n_tiles, 0);
    refining_ = 0;
    polish_.assign(n_tiles, kUnpolished);
    polish_owed_ = false;
    born_.assign(n_tiles, 0);
    hot_us_.assign(n_tiles, 0);
    quality_.assign(n_tiles, kRough);
    tile_slot_.assign(n_tiles, -1);
    tile_slot_hash_.assign(n_tiles, 0);
    pending_lossless_.assign(n_tiles, 0);
    pending_lossless_count_ = 0;
    unflattened_.assign(n_tiles, 0);
    stale_.assign(n_tiles, 0);
    coarse_.assign(n_tiles, 0);
  }
  // A keyframe starts the progressive sequence over.
  const bool progressive_key = key && options_.progressive && options_.lossless;
  if (key) {
    std::fill(pending_lossless_.begin(), pending_lossless_.end(), 0);
    pending_lossless_count_ = 0;
    pending_cursor_ = 0;
    std::fill(unflattened_.begin(), unflattened_.end(), 0);
    std::fill(stale_.begin(), stale_.end(), 0);
    std::fill(coarse_.begin(), coarse_.end(), 0);
    urgent_.clear();
    progressive_active_ = progressive_key;
    sharpen_video_ = false;
    progressive_last_us_ = frame.capture_time_us;
  }
  ++frame_no_;

  // Video areas: their tiles leave the normal path and go to the area's own
  // stream (see EncodeRegions).
  const bool text = options_.text_mode && options_.lossless;
  // (Video areas as without text mode: a film in its own cropped stream is
  // both smoother and cheaper than in full-screen pictures.)
  const bool use_regions = options_.region_encoder_factory && !regions_disabled_;
  if (regions_.cols() != differ_.cols() || regions_.rows() != differ_.rows()) {
    regions_.Reset(width_, height_);
  }
  if (key) {
    regions_.Clear();
    region_enc_.clear();
    pending_sharpen_.clear();
  }
  const size_t n_regions = regions_.regions().size();
  std::vector<bool> region_dirty(n_regions, false);
  std::vector<uint8_t> busy(use_regions ? n_tiles : 0, 0);
  std::vector<uint8_t> region_changed(use_regions ? n_tiles : 0, 0);
  bool region_needs_key = false;
  for (size_t k = 0; k < n_regions; ++k) {
    const auto it = region_enc_.find(regions_.regions()[k].stream);
    if (it == region_enc_.end() || it->second.need_key) region_needs_key = true;
    // The viewer must never take these tiles from the full-screen picture,
    // which no longer follows them.
    for (size_t idx : TilesOf(regions_.regions()[k].rect)) {
      if (refine_[idx] > 0) {
        refine_[idx] = 0;
        --refining_;
      }
    }
  }
  if (n_regions > 0) {
    std::erase_if(changes, [&](const tile::TileChange& c) {
      const int k = regions_.RegionOf(c.col, c.row);
      if (k < 0) return false;
      const size_t idx = static_cast<size_t>(c.row) * differ_.cols() + c.col;
      region_dirty[static_cast<size_t>(k)] = true;
      busy[idx] = region_changed[idx] = 1;
      CopyRectInto(frame.bgra, c.rect, prev_.data(), width_ * 4);
      quality_[idx] = kRough;
      born_[idx] = frame_no_;
      stale_[idx] = 0;  // the area's picture brings it up to date
      return true;
    });
  }
  const bool regions_to_send =
      region_needs_key || std::find(region_dirty.begin(), region_dirty.end(), true) != region_dirty.end();

  // The lossless bucket fills with time, also over frames that end up
  // skipped below: owed tiles are picked by what it holds, so a still screen
  // must keep filling it or they would wait for the next change.
  {
    const double frame_us = 1e6 / std::max(1, options_.fps);
    // Nothing changed at all (a still screen, typically the Host re-sending
    // the last frame every 100 ms because something is owed): the video
    // sends nothing, so the owed tiles may use most of the link, and the
    // bucket holds one re-send's worth.
    const bool still = changes.empty() && !key && !holding_back_;
    // Text mode: 150 ms of it, so that what a key press changes goes out
    // at once after a pause (at 100 kbps three frame intervals hold 1.2 kB,
    // less than one tile of text).
    const double burst_us = text ? std::max(kLosslessBurstFrames * frame_us, 300'000.0)
                            : still ? std::max(kLosslessBurstFrames * frame_us, 100'000.0)
                                    : kLosslessBurstFrames * frame_us;
    const double cap = static_cast<double>(options_.bitrate_bps) / 8.0 * burst_us / 1e6;
    if (!lossless_tokens_set_) {
      lossless_tokens_ = cap;
      lossless_tokens_set_ = true;
    } else {
      // Owed tiles (a progressive keyframe, or tiles the budget sent as
      // video) get a larger share while the screen is nearly still, unless
      // frames are being held back: then the path is full already.
      // While the screen keeps moving (a scroll, a drag), lossless gets less,
      // so what is uncovered goes as video first and turns sharp once the
      // screen stops: at 0.4 Mbps a scroll's new rows of text alone need
      // more than the link, and showing every frame beats showing them exact
      // while they fly past.
      const double share = text ? kTextLosslessShare
                           : moving_ ? kLosslessMovingShare
                           : (progressive_active_ || still) && !holding_back_ ? kProgressiveShare
                                                                              : kLosslessRateShare;
      const double gap_us = static_cast<double>(frame.capture_time_us -
                                                std::min(frame.capture_time_us, lossless_tokens_us_));
      lossless_tokens_ = std::min(
          cap, lossless_tokens_ + static_cast<double>(options_.bitrate_bps) / 8.0 * share * gap_us / 1e6);
    }
    lossless_tokens_us_ = frame.capture_time_us;
    lossless_cap_ = cap;
  }
  // Text mode: a screenful is owed, so tiles get their quick pass first.
  const bool coarse_now =
      text && pending_lossless_count_ > kTextCoarseBacklog &&
      static_cast<double>(pending_lossless_count_) * std::max(lossless_bytes_per_tile_, 64.0) >
          kTextCoarseBacklogSeconds * static_cast<double>(options_.bitrate_bps) / 8.0 * kTextLosslessShare;
  // Tiles the viewer shows at first-pass video quality (or copies of such)
  // that have been static for a while get sent again properly.
  std::vector<size_t> refresh;
  if (!key && options_.refresh) {
    for (size_t i = 0; i < n_tiles && refresh.size() < kMaxRefreshPerFrame; ++i) {
      if (quality_[i] == kRough && refine_[i] == 0 && frame_no_ - born_[i] >= kRefreshDelay) {
        refresh.push_back(i);
      }
    }
  }
  // Progressive keyframe: the next lossless tiles, as many as fit about
  // half of this frame's share of the bitrate.
  size_t progressive_sent = 0;
  std::vector<uint8_t> progressive_now(n_tiles, 0);
  // Once the screen is nearly still, every tile left at video quality (sent
  // as video for want of budget, moved or copied from such a tile, or the
  // first picture's) is owed too, and goes out losslessly within the bucket
  // like the rest; one that is not palette content goes as video and is
  // refined by the encoder instead. The user's choice for a slow link:
  // blurry while it moves, sharp once it stops.
  // (Text mode does not wait for a still screen: nothing there is taken
  // from the video meanwhile, and a clock or a spinner somewhere must not
  // keep the rest at its quick pass.)
  if (!key && (quiet_ || text) && options_.lossless) {
    for (size_t i = 0; i < n_tiles; ++i) {
      if (quality_[i] != kRough || refine_[i] != 0 || pending_lossless_[i] ||
          frame_no_ - born_[i] < kRefreshDelay) {
        continue;
      }
      const auto col = static_cast<int32_t>(i % differ_.cols());
      const auto row = static_cast<int32_t>(i / differ_.cols());
      if (regions_.RegionOf(col, row) >= 0) continue;
      pending_lossless_[i] = 1;
      ++pending_lossless_count_;
    }
  }
  // Tiles the budget sent as video wait for a nearly still screen (they
  // would take from the video otherwise); a progressive keyframe goes on.
  // In text mode they were not sent as video, so they need not wait.
  if (!key && pending_lossless_count_ > 0 && (progressive_active_ || quiet_ || text)) {
    std::vector<uint8_t> listed(n_tiles, 0);
    for (const auto& c : changes) listed[static_cast<size_t>(c.row) * differ_.cols() + c.col] = 1;
    for (size_t idx : refresh) listed[idx] = 1;
    // By time rather than per frame: frames the transport made us skip
    // leave their share to the next one.
    const double frame_us = 1e6 / std::max(1, options_.fps);
    const double dt_us = std::clamp(
        static_cast<double>(frame.capture_time_us - std::min(frame.capture_time_us, progressive_last_us_)),
        frame_us, 6 * frame_us);
    const double budget =
        static_cast<double>(options_.bitrate_bps) / 8.0 * dt_us / 1e6 * kProgressiveShare;
    // Within the lossless bucket too: a progressive keyframe right after a
    // drop would otherwise send its tiles faster than the path takes them.
    const double per_tile = std::max(lossless_bytes_per_tile_, 64.0);
    const double fits = std::min(budget, std::max(0.0, lossless_tokens_)) / per_tile;
    // At least one once the bucket holds a tile's worth, or the frame's
    // share, or is full: on a slow link one tile of detailed text can cost
    // more than the bucket holds, and waiting for more would wait forever.
    const bool one = lossless_tokens_ >= per_tile || lossless_tokens_ >= budget ||
                     lossless_tokens_ >= 0.999 * lossless_cap_;
    const auto n = static_cast<size_t>(std::clamp(
        fits, one ? 1.0 : 0.0,
        static_cast<double>(kMaxProgressivePerFrame)));
    // Text mode: first what was deferred as it changed, newest first.
    while (!urgent_.empty() && progressive_sent < n) {
      const size_t idx = urgent_.back();
      urgent_.pop_back();
      if (idx >= n_tiles || !pending_lossless_[idx] || listed[idx] || progressive_now[idx]) continue;
      refresh.push_back(idx);
      progressive_now[idx] = 1;
      ++progressive_sent;
    }
    const size_t start = pending_cursor_;
    for (size_t k = 0; k < n_tiles && progressive_sent < n && pending_lossless_count_ > 0; ++k) {
      const size_t idx = (start + k) % n_tiles;
      if (!pending_lossless_[idx] || progressive_now[idx]) continue;
      if (listed[idx]) continue;  // changed anyway: coded normally below
      const auto col = static_cast<int32_t>(idx % differ_.cols());
      const auto row = static_cast<int32_t>(idx / differ_.cols());
      if (regions_.RegionOf(col, row) >= 0) {
        // Became video: it will never be lossless.
        pending_lossless_[idx] = 0;
        --pending_lossless_count_;
        continue;
      }
      refresh.push_back(idx);
      progressive_now[idx] = 1;
      ++progressive_sent;
      pending_cursor_ = idx + 1;
    }
    if (progressive_sent > 0) progressive_last_us_ = frame.capture_time_us;
  }
  // Tiles of areas that just closed get one more, sharp, picture.
  if (!pending_sharpen_.empty()) {
    std::vector<uint8_t> listed(n_tiles, 0);
    for (const auto& c : changes) listed[static_cast<size_t>(c.row) * differ_.cols() + c.col] = 1;
    for (size_t idx : refresh) listed[idx] = 1;
    for (size_t idx : pending_sharpen_) {
      const auto col = static_cast<int32_t>(idx % differ_.cols());
      const auto row = static_cast<int32_t>(idx / differ_.cols());
      if (idx < n_tiles && !listed[idx] && regions_.RegionOf(col, row) < 0) {
        listed[idx] = 1;
        refresh.push_back(idx);
      }
    }
    pending_sharpen_.clear();
  }
  // Tiles at video quality that the still-screen polish has yet to do.
  // (Text mode polishes as without it: with half the pictures, photos,
  // icons and the wallpaper stayed softer than in the normal mode.)
  const auto still_frames = static_cast<uint8_t>(
      options_.still_min_qp > 0 && options_.refine_frames > 0 ? std::clamp(options_.still_refine_frames, 0, static_cast<int>(kUnpolished) - 1) : 0);
  for (const auto& c : changes) polish_[static_cast<size_t>(c.row) * differ_.cols() + c.col] = kUnpolished;
  polish_owed_ = false;
  if (still_frames > 0 && !key) {
    for (size_t i = 0; i < n_tiles && !polish_owed_; ++i) {
      if (polish_[i] == 0) continue;
      if (quality_[i] == kExact) {
        polish_[i] = 0;
        continue;
      }
      const auto col = static_cast<int32_t>(i % differ_.cols());
      const auto row = static_cast<int32_t>(i / differ_.cols());
      polish_owed_ = regions_.RegionOf(col, row) < 0;  // areas have encoders of their own
    }
  }
  if (changes.empty() && !key && refining_ == 0 && refresh.empty() && !regions_to_send &&
      !sharpen_video_ && !polish_owed_ && (!text || carry_video_tiles_.empty())) {
    quiet_ = true;
    moving_ = false;
    if (use_regions) FinishRegions(busy, region_changed);
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.frames_skipped;
    return true;
  }

  const uint64_t t0 = capture::MonotonicMicros();
  const int32_t enc_stride = width_ * 4;
  auto tile_index = [&](const tile::TileChange& c) {
    return static_cast<size_t>(c.row) * differ_.cols() + c.col;
  };

  // Tile cache, part 1: before this frame's updates, keep what the viewer
  // shows for tiles that are about to change, if that content is likely to
  // come back: it was on screen for a while (a window about to be covered),
  // or it has been seen before (a cycling animation).
  std::vector<codec::SlotOp> stores;
  if (key) cache_.Clear();
  if (!key && options_.tile_cache) {
    // Recurrence alone is a weak signal (scrolling by a multiple of the tile
    // size makes every tile "recur"), so only a few such stores per frame.
    size_t recurring_budget = kMaxRecurringStores;
    for (const auto& c : changes) {
      if (c.prev_hash == 0) continue;
      const bool stable = frame_no_ - born_[tile_index(c)] >= kStableFrames;
      bool recurring = false;
      if (!stable && recurring_budget > 0) {
        auto seen = seen_.find(c.prev_hash);
        recurring = seen != seen_.end() && seen->second >= 2;
      }
      // (Not what the viewer does not have: see stale_.)
      if ((stable || recurring) && !cache_.Find(c.prev_hash) && !stale_[tile_index(c)]) {
        if (!stable) --recurring_budget;
        const uint32_t slot = cache_.Insert(c.prev_hash);
        slot_quality_[slot] = quality_[tile_index(c)];
        stores.push_back({slot, c.rect});
        // prev_ still holds the previous frame here, i.e. the stored content.
        constexpr size_t kSlotBytes = static_cast<size_t>(kTileSize) * kTileSize * 4;
        if (slot_pixels_.empty()) slot_pixels_.resize(proto::kTileCacheSlots * kSlotBytes);
        for (int32_t y = 0; y < c.rect.h; ++y) {
          std::memcpy(slot_pixels_.data() + slot * kSlotBytes + static_cast<size_t>(y) * kTileSize * 4,
                      prev_.data() + static_cast<size_t>(c.rect.y + y) * enc_stride + c.rect.x * 4,
                      static_cast<size_t>(c.rect.w) * 4);
        }
        tile_slot_[tile_index(c)] = slot;
        tile_slot_hash_[tile_index(c)] = c.prev_hash;
      }
    }
  }
  if (seen_.size() > kMaxSeen) seen_.clear();
  // Time since the previous frame, at least one frame interval: longer when
  // the transport held frames back (or nothing changed for a while).
  const double frame_us = 1e6 / std::max(1, options_.fps);
  const double since_us =
      last_frame_us_ == 0 ? frame_us
                          : std::clamp(static_cast<double>(frame.capture_time_us -
                                                           std::min(frame.capture_time_us, last_frame_us_)),
                                       frame_us, 6 * frame_us);
  last_frame_us_ = frame.capture_time_us;
  // Frames are being held back when several came late within a second (one
  // late frame is just the first change after a quiet spell).
  if (since_us > 1.5 * frame_us) {
    std::rotate(late_us_.begin(), late_us_.begin() + 1, late_us_.end());
    late_us_.back() = frame.capture_time_us;
  }
  const bool holding_back =
      late_us_.front() != 0 && frame.capture_time_us - late_us_.front() <= 1'000'000;
  holding_back_ = holding_back;
  std::vector<bool> hot(changes.size());
  for (size_t i = 0; i < changes.size(); ++i) {
    uint8_t& n = seen_[changes[i].hash];
    if (n < 255) ++n;
    uint64_t& born = born_[tile_index(changes[i])];
    // Hot: changed in the previous frame too, or, while frames are being
    // held back, hot not long ago. A tile that is only partly video (the
    // edge of a video window) does not change in every frame, and coded
    // losslessly in between (up to 256 colours) it cost ~1 kB a time:
    // 10-20 kB frames, 0.3 s of a 0.4 Mbps link each, which held back the
    // frames after them, so more tiles missed a frame. (On a fast path the
    // hold would only cost text that scrolls: it went as video more often.)
    uint64_t& hot_us = hot_us_[tile_index(changes[i])];
    hot[i] = !key && (frame_no_ - born <= 1 || (holding_back && hot_us != 0 &&
                                                frame.capture_time_us - hot_us <= kHotHoldUs));
    hot_us = hot[i] ? frame.capture_time_us : 0;
    born = frame_no_;
  }

  // Text mode: many tiles that change frame after frame are motion (a
  // scroll that re-renders its text, a window zooming to full screen, a
  // film), not typing. Motion goes through the video encoder as a whole,
  // as without text mode: every tile of it in the same picture, a little
  // soft, and what is text is sent again exactly once it stops. Deciding
  // tile by tile (this one sharp now, that one later, a third as video)
  // showed the screen cut into pieces of different ages for as long as it
  // moved. A few such tiles (typing, a caret, a clock) stay exact.
  constexpr size_t kTextMaxHotTiles = 8;
  size_t n_hot = 0;
  for (bool h : hot) n_hot += h;
  bool mass_motion = text && n_hot > kTextMaxHotTiles;

  std::vector<tile::Move> moves;
  std::vector<bool> moved;
  std::vector<std::vector<Rect>> residual;
  if (!key && options_.detect_moves) {
    moves = tile::DetectMoves({prev_.data(), width_, height_, enc_stride}, frame.bgra, changes,
                              moved, residual);
    // Copies are cheap one at a time, but content that moves while it
    // changes (a video panning over a detailed picture) matches in a
    // thousand strips, each a copy plus slivers to code: 6-8 kB a frame
    // where video takes ~1 kB, which at 0.4 Mbps backed frames up for most
    // of a second. Such matches come in many pieces per tile (a scroll
    // brings fewer copies than tiles, a dragged window up to ~3 per tile);
    // when they do and their copies alone would take more than half of what
    // the lossless budget holds, the frame goes without them.
    constexpr double kBytesPerMove = 6;
    // Text mode: a tile that keeps changing and gets less than half of its
    // pixels from copies has not moved; something in it has (a film that
    // pans: its tiles match a strip here and there). It is coded as a
    // whole.
    if (text) {
      for (size_t i = 0; i < changes.size(); ++i) {
        if (!moved[i] || !hot[i]) continue;
        int64_t left = 0;
        for (const Rect& r : residual[i]) left += static_cast<int64_t>(r.w) * r.h;
        if (left * 2 > static_cast<int64_t>(changes[i].rect.w) * changes[i].rect.h) {
          moved[i] = false;
          residual[i].clear();
        }
      }
    }
    size_t moved_count = 0;
    for (bool b : moved) moved_count += b;
    // (Tiles that are copied are not the ones in question.)
    size_t hot_unmoved = 0;
    for (size_t i = 0; i < changes.size(); ++i) hot_unmoved += hot[i] && !moved[i];
    if (hot_unmoved <= kTextMaxHotTiles) mass_motion = false;
    const double hold = static_cast<double>(options_.bitrate_bps) / 8.0 * kLosslessBurstFrames * frame_us / 1e6;
    if (moves.size() > 64 && moves.size() > 6 * moved_count && options_.bitrate_bps > 0 &&
        kBytesPerMove * static_cast<double>(moves.size()) > 0.5 * hold) {
      moves.clear();
      moved.assign(moved.size(), false);
      for (auto& r : residual) r.clear();
    }
    // The encoder's picture is left as it was under moved tiles: the viewer
    // does not take video there, and unchanged pixels cost the encoder
    // nothing, while moved ones would make it code the motion.
  }
  // A moved rect is as good as the worst tile it was copied from.
  auto quality_under = [&](const Rect& r) {
    uint8_t q = kExact;
    for (int32_t row = r.y / kTileSize; row <= (r.bottom() - 1) / kTileSize; ++row) {
      for (int32_t col = r.x / kTileSize; col <= (r.right() - 1) / kTileSize; ++col) {
        q = std::min(q, quality_[static_cast<size_t>(row) * differ_.cols() + col]);
      }
    }
    return q;
  };
  // Text mode: a rect copied out of a tile the viewer does not have would
  // paint its placeholder (or its old content) over a tile that is fine.
  // Such copies are not sent; the tiles they would have filled are coded
  // whole instead (or wait, keeping what they show).
  std::vector<uint8_t> unmoved;
  if (text && !moves.empty()) {
    bool any = false;
    std::vector<uint8_t> from_stale(moves.size(), 0);
    for (size_t k = 0; k < moves.size(); ++k) {
      const Rect r = moves[k].src();
      for (int32_t row = r.y / kTileSize; row <= (r.bottom() - 1) / kTileSize && !from_stale[k]; ++row) {
        for (int32_t col = r.x / kTileSize; col <= (r.right() - 1) / kTileSize; ++col) {
          if (stale_[static_cast<size_t>(row) * differ_.cols() + col] & kStalePlaceholder) {
            from_stale[k] = 1;
            break;
          }
        }
      }
      any |= from_stale[k] != 0;
    }
    if (any) {
      unmoved.assign(n_tiles, 0);
      std::vector<tile::Move> kept;
      for (size_t k = 0; k < moves.size(); ++k) {
        if (!from_stale[k]) {
          kept.push_back(moves[k]);
          continue;
        }
        const Rect& d = moves[k].dst;
        for (int32_t row = d.y / kTileSize; row <= (d.bottom() - 1) / kTileSize; ++row) {
          for (int32_t col = d.x / kTileSize; col <= (d.right() - 1) / kTileSize; ++col) {
            unmoved[static_cast<size_t>(row) * differ_.cols() + col] = 1;
          }
        }
      }
      moves = std::move(kept);
    }
  }
  std::vector<uint8_t> move_quality;
  for (const auto& m : moves) move_quality.push_back(quality_under(m.src()));
  // Whether a moved rect copies from a tile the viewer has older content
  // for: what it fills is then owed as well.
  std::vector<uint8_t> move_tainted(moves.size(), 0);
  bool any_tainted = false;
  if (text) {
    for (size_t k = 0; k < moves.size(); ++k) {
      const Rect r = moves[k].src();
      for (int32_t row = r.y / kTileSize; row <= (r.bottom() - 1) / kTileSize; ++row) {
        for (int32_t col = r.x / kTileSize; col <= (r.right() - 1) / kTileSize; ++col) {
          move_tainted[k] |= stale_[static_cast<size_t>(row) * differ_.cols() + col];
        }
      }
      any_tainted |= move_tainted[k] != 0;
    }
  }
  auto moved_in_tainted = [&](const Rect& tile_rect) {
    if (!any_tainted) return false;
    for (size_t k = 0; k < moves.size(); ++k) {
      const Rect& d = moves[k].dst;
      if (move_tainted[k] && d.x < tile_rect.right() && tile_rect.x < d.right() && d.y < tile_rect.bottom() &&
          tile_rect.y < d.bottom()) {
        return true;
      }
    }
    return false;
  };
  // Quality of what each moved tile receives from the moves.
  auto moved_in_quality = [&](const Rect& tile_rect) {
    uint8_t q = kExact;
    for (size_t k = 0; k < moves.size(); ++k) {
      const Rect& d = moves[k].dst;
      if (d.x < tile_rect.right() && tile_rect.x < d.right() && d.y < tile_rect.bottom() &&
          tile_rect.y < d.bottom()) {
        q = std::min(q, move_quality[k]);
      }
    }
    return q;
  };
  size_t moved_tiles = 0;
  std::vector<codec::SlotOp> loads;
  std::vector<codec::PaletteTile> lossless_tiles;
  std::vector<tile::TileChange> video_tiles;  // video coded, whole or in part
  const auto refine_frames = static_cast<uint8_t>(std::clamp(options_.refine_frames, 0, 255));
  auto set_refine = [&](const tile::TileChange& c, uint8_t v) {
    uint8_t& r = refine_[tile_index(c)];
    refining_ += (v > 0) - (r > 0);
    r = v;
    // Coded as video (again): the still-screen polish is owed afresh.
    if (v > 0) polish_[tile_index(c)] = kUnpolished;
  };
  // What the lossless part of this frame may cost: a few frame intervals of
  // the bitrate (more after frames were held back). Content that turns up
  // with many colours at once (a video that paused for a moment, a window
  // of photos) would otherwise go out in one burst: 20 kB is 0.4 s of a
  // 0.4 Mbps link, and every frame after it waits. Tiles beyond the budget
  // go as video now; once static they are sent again losslessly (refresh),
  // within the budget of those frames.
  // A text mode keyframe may take half a second of the bitrate: the whole
  // screen's text then goes out with it on any path that is not slow, and
  // no tile is left a placeholder (while some are, everything a scroll or
  // a dragged window touches has to be coded whole, frame after frame).
  const double lossless_budget =
      text && key ? std::max(lossless_tokens_, static_cast<double>(options_.bitrate_bps) / 8.0 * 0.5)
                  : lossless_tokens_;
  double lossless_spent = 0;  // estimated
  double lossless_raw = 0;    // raw size of the tiles that go out
  bool over_lossless_budget = false;
  auto owe_lossless = [&](size_t idx) {
    if (!pending_lossless_[idx]) {
      pending_lossless_[idx] = 1;
      ++pending_lossless_count_;
    }
  };
  // Codes `r` (all or part of tile `c`) losslessly if it has few colours,
  // else through the video encoder. Returns kCodedLossless, kCodedVideo, or
  // (text mode) kDeferred: nothing was sent, the tile is stale and owed.
  enum : int { kCodedLossless = 0, kCodedVideo = 1, kDeferred = 2 };
  bool last_coarse = false;  // the last rect went out at the quick pass's colours
  auto code_rect = [&](const tile::TileChange& c, const Rect& r, bool is_hot) -> int {
    // Lossless pays off on content that stays, or on a thin sliver; content
    // that keeps changing is cheaper as video unless it is very simple.
    const bool sliver = r.h * 4 <= kTileSize || r.w * 4 <= kTileSize;
    int max_colors =
        is_hot && !sliver ? options_.lossless_max_colors_hot : options_.lossless_max_colors;
    // Narrow vertical slivers (the sides of a dragged window) are new every
    // frame and each carries its own palette, which then outweighs the pixels
    // (3 bytes a colour). Horizontal slivers are mostly the rows a scroll
    // uncovers, which stay on screen and are worth keeping exact.
    if (is_hot && r.w < r.h) {
      max_colors = std::max(std::min(max_colors, r.w * r.h / 6), options_.lossless_max_colors_hot);
    }
    codec::PaletteTile t;
    // Text mode: reduced colours. What changes every frame is exact while it
    // is a few tiles (typing must not turn to video); as part of motion
    // (see mass_motion) it follows the rule for hot tiles: exact with very
    // few colours, else video now and, as it may be text, owed exactly.
    bool palette = false;
    last_coarse = false;
    const bool in_motion = text && mass_motion && is_hot && !sliver && !key &&
                           (progressive_now.empty() || !progressive_now[tile_index(c)]);
    if (in_motion) {
      palette = codec::ExtractPalette(frame.bgra, r, max_colors, t);
      if (!palette) owe_lossless(tile_index(c));
    } else if (text) {
      const size_t idx = tile_index(c);
      // The quick pass: a whole tile on its turn, which the viewer has
      // nothing useful for (stale, or a keyframe's), once.
      if (coarse_now && !progressive_now.empty() && progressive_now[idx] && !coarse_[idx] &&
          (progressive_active_ || (stale_[idx] & kStaleText))) {
        palette = codec::ExtractReducedPalette(frame.bgra, r, kTextCoarseColors, options_.lossless_max_colors,
                                               4096, t, &last_coarse);
      }
      if (!palette) {
        last_coarse = false;
        // Faithful: text in several colours gets 32 or 64 instead of going
        // to the video encoder (two thirds of a page of highlighted code
        // on a dark background did, at 16).
        palette = codec::ExtractReducedPalette(frame.bgra, r, options_.text_colors,
                                               options_.lossless_max_colors, 4096, t, nullptr, /*faithful=*/true);
        // Not text (a gradient, a wallpaper, an icon): the video encoder's,
        // which is good at these and polishes them once the screen is
        // still. (Exact, as without text mode for up to 256 colours, such a
        // tile costs 2-3 kB; a scroll over a textured background came to
        // more traffic than without text mode.)
      }
    } else {
      palette = options_.lossless && codec::ExtractPalette(frame.bgra, r, max_colors, t);
    }
    if (palette) {
      // (Text mode has no video-first keyframe: its tiles go out with the
      // keyframe as far as the budget reaches, see below.)
      if (progressive_key && !text && t.colors.size() > 1) {
        // Video now, lossless later (only whole tiles are keyframe changes).
        // Plain tiles cost next to nothing either way and go right away.
        owe_lossless(tile_index(c));
        CopyRectInto(frame.bgra, r, enc_input_.data(), enc_stride);
        tile::TileChange part = c;
        part.rect = r;
        video_tiles.push_back(part);
        return kCodedVideo;
      }
      const bool progressive_turn = !progressive_now.empty() && progressive_now[tile_index(c)];
      const size_t n = t.colors.size();
      const int bits = n <= 1 ? 0 : n <= 2 ? 1 : n <= 4 ? 2 : n <= 16 ? 4 : 8;
      // Plus the rect and the palette size (a plain sliver costs that alone).
      const double raw = 3.0 * static_cast<double>(n) + static_cast<double>(r.w) * r.h * bits / 8.0;
      // Text mode counts a tile at what tiles have been deflating to, and
      // lets the first of a frame through while the bucket is not overdrawn
      // (at these rates one tile is more than a frame's share).
      const double cost = text ? raw * lossless_ratio_ : raw * kLosslessRawShare;
      const bool over = text ? lossless_budget <= 0 || (lossless_spent > 0 && lossless_spent + cost > lossless_budget)
                             : lossless_spent + cost > lossless_budget;
      if ((!key || text) && !progressive_turn && n > 1 && options_.bitrate_bps > 0 && over) {
        over_lossless_budget = true;
        // Owed: sent losslessly once it stops changing, within the bucket
        // (the same queue as a progressive keyframe's tiles). Without this
        // the tile stayed at video quality (refresh is off by default).
        const size_t idx = tile_index(c);
        owe_lossless(idx);
        // Text mode waits only where it must: with a keyframe (whose
        // picture shows the tile's background meanwhile). Otherwise as
        // without text mode, video now and exact later: a tile left as it
        // was among tiles that are new looks worse than one that is soft for
        // a moment.
        if (text && key) {
          stale_[idx] |= kStaleText;
          if (key) {
            stale_[idx] |= kStalePlaceholder;
            // A keyframe's picture covers the whole screen: there the tile
            // is its background colour until its text comes (on a path
            // that takes the screen at once, none is left like that). The
            // viewer does not have such a tile: a caret blinking in it
            // before its turn stored the placeholder in the tile cache and
            // painted it back at every blink, and a partial change left
            // the rest of it a block of plain colour for good.
            FillRect(r, t.colors[0], enc_input_.data(), enc_stride);
          } else if (urgent_.size() < 4096) {
            urgent_.push_back(idx);
          }
          return kDeferred;
        }
        CopyRectInto(frame.bgra, r, enc_input_.data(), enc_stride);
        tile::TileChange part = c;
        part.rect = r;
        video_tiles.push_back(part);
        return kCodedVideo;
      }
      lossless_spent += cost;
      lossless_raw += raw;
      if (progressive_turn) {
        // Flattening it now would make the encoder code the change, and at a
        // low bitrate that costs the tiles still refining around it.
        unflattened_[tile_index(c)] = 1;
      } else {
        FillRect(r, t.colors[0], enc_input_.data(), enc_stride);
      }
      lossless_tiles.push_back(std::move(t));
      return kCodedLossless;
    }
    CopyRectInto(frame.bgra, r, enc_input_.data(), enc_stride);
    tile::TileChange part = c;
    part.rect = r;
    video_tiles.push_back(part);
    return kCodedVideo;
  };
  // The previous frame's pixels of the tile being processed (prev_ is updated
  // as tiles are processed).
  std::vector<uint8_t> prev_tile(static_cast<size_t>(kTileSize) * kTileSize * 4);
  // Paints the parts of `r` (inside tile `c`) that equal what the tile's last
  // stored slot holds from that slot, in 16-pixel strips. Returns the parts
  // still to code; lowers `q` to the slot's quality if the slot was used.
  // Gives up (returns `r` whole) when less than `min_share` of it matches.
  auto paint_from_slot = [&](const tile::TileChange& c, const Rect& r, double min_share,
                             uint8_t& q) {
    std::vector<Rect> rest;
    const size_t idx = tile_index(c);
    const int64_t slot = tile_slot_[idx];
    if (key || !options_.tile_cache || slot < 0 ||
        !cache_.Holds(static_cast<uint32_t>(slot), tile_slot_hash_[idx])) {
      return std::vector<Rect>{r};
    }
    constexpr size_t kSlotBytes = static_cast<size_t>(kTileSize) * kTileSize * 4;
    constexpr int32_t kStrip = 16;
    const uint8_t* base = slot_pixels_.data() + static_cast<size_t>(slot) * kSlotBytes;
    const size_t first_load = loads.size();
    int64_t matched = 0;
    for (int32_t x0 = r.x; x0 < r.right(); x0 += kStrip) {
      const int32_t sw = std::min(kStrip, r.right() - x0);
      // 'U' unchanged since the previous frame (the viewer already shows it),
      // 'S' from the slot, 'X' to code.
      auto kind = [&](int32_t y) {
        const uint8_t* cur = frame.bgra.data + static_cast<size_t>(y) * frame.bgra.stride + x0 * 4;
        const size_t off = (static_cast<size_t>(y - c.rect.y) * kTileSize + (x0 - c.rect.x)) * 4;
        const size_t n = static_cast<size_t>(sw) * 4;
        if (std::memcmp(cur, prev_tile.data() + off, n) == 0) return 'U';
        if (std::memcmp(cur, base + off, n) == 0) return 'S';
        return 'X';
      };
      for (int32_t y = r.y; y < r.bottom();) {
        const char k = kind(y);
        int32_t e = y + 1;
        while (e < r.bottom() && kind(e) == k) ++e;
        const Rect run{x0, y, sw, e - y};
        if (k == 'U') {
          // nothing to send
        } else if (k == 'S') {
          loads.push_back({static_cast<uint32_t>(slot), run, static_cast<uint32_t>(x0 - c.rect.x),
                           static_cast<uint32_t>(y - c.rect.y)});
          matched += static_cast<int64_t>(run.w) * run.h;
        } else {
          rest.push_back(run);
        }
        y = e;
      }
    }
    if (matched == 0 || matched < min_share * r.w * r.h) {
      loads.resize(first_load);
      return std::vector<Rect>{r};
    }
    q = std::min(q, slot_quality_[slot]);
    // Join strips that cover the same rows.
    std::sort(rest.begin(), rest.end(),
              [](const Rect& a, const Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    std::vector<Rect> merged;
    for (const Rect& p : rest) {
      if (!merged.empty() && merged.back().y == p.y && merged.back().h == p.h &&
          merged.back().right() == p.x) {
        merged.back().w += p.w;
      } else {
        merged.push_back(p);
      }
    }
    return merged;
  };
  // Refreshed tiles are coded like changed ones (their content is current in
  // `frame`, as they did not change).
  for (size_t idx : refresh) {
    const auto col = static_cast<int32_t>(idx % differ_.cols());
    const auto row = static_cast<int32_t>(idx / differ_.cols());
    Rect rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize};
    rect.w = std::min(rect.w, width_ - rect.x);
    rect.h = std::min(rect.h, height_ - rect.y);
    changes.push_back({col, row, 0, 0, rect});
  }
  const size_t n_changed = changes.size() - refresh.size();
  quiet_ = n_changed * 50 <= n_tiles;
  std::vector<size_t> refine_after;  // tiles that went to video whole
  std::vector<uint8_t> recoded(text ? n_tiles : 0, 0);
  for (size_t i = 0; i < changes.size(); ++i) {
    const auto& c = changes[i];
    const size_t idx = tile_index(c);
    // The viewer does not have the tile as it was (text mode): nothing in
    // it can be taken as unchanged, so it is coded whole.
    const bool was_stale = stale_[idx] != 0;
    if (text) recoded[idx] = 1;
    if (i < n_changed) coarse_[idx] = 0;  // new content
    if (i < n_changed) {
      for (int32_t y = 0; y < c.rect.h; ++y) {
        std::memcpy(prev_tile.data() + static_cast<size_t>(y) * kTileSize * 4,
                    prev_.data() + static_cast<size_t>(c.rect.y + y) * enc_stride + c.rect.x * 4,
                    static_cast<size_t>(c.rect.w) * 4);
      }
      CopyRectInto(frame.bgra, c.rect, prev_.data(), enc_stride);
    }
    // Whatever happens below, the viewer must not take this tile from a
    // refining video picture any more (lossless tiles are flattened in it).
    set_refine(c, 0);
    if (!key && pending_lossless_[idx]) {
      // Coded now (changed, or its progressive turn): nothing owed any more.
      pending_lossless_[idx] = 0;
      --pending_lossless_count_;
    }
    if (i < n_changed && !moved.empty() && moved[i] && !(stale_[idx] & kStalePlaceholder) &&
        !(!unmoved.empty() && unmoved[idx])) {
      ++moved_tiles;
      uint8_t q = moved_in_quality(c.rect);
      if (was_stale || moved_in_tainted(c.rect)) {
        // The copies go (see kStalePlaceholder); the tile is owed whole.
        stale_[idx] |= kStaleText;
        owe_lossless(idx);
        q = kRough;
      }
      if (!residual[i].empty()) q = std::min(q, quality_[idx]);  // rows that stayed put
      // Only the rows that did not move or stay put are left to code, and of
      // those, what a dragged window just uncovered can come from the cache.
      for (const Rect& r : residual[i]) {
        for (const Rect& part : paint_from_slot(c, r, 0.0, q)) {
          if (code_rect(c, part, hot[i])) q = kRough;
        }
      }
      quality_[idx] = q;
      // Text mode: what was copied from video quality may be text that went
      // as video while it moved; it is owed exactly like the tile it came
      // from (which is not where it was any more).
      if (text && q != kExact) owe_lossless(idx);
      continue;
    }
    // Tile cache, part 2: content the viewer has kept. Like moves, the
    // encoder's picture is left alone here.
    if (i < n_changed && !key && options_.tile_cache) {
      if (auto slot = cache_.Find(c.hash)) {
        loads.push_back({*slot, c.rect});
        quality_[idx] = slot_quality_[*slot];
        stale_[idx] = 0;
        if (text && quality_[idx] != kExact) owe_lossless(idx);
        continue;
      }
    }
    // A tile that was static and now changes partly (typing into a line
    // that was stored, a menu closing) may get most of its pixels from the
    // slot; hot tiles (video, animation) are left whole.
    if (i < n_changed && !hot[i] && !was_stale) {
      // What stays of a rough tile stays rough (it is sent again once the
      // screen is still); calling it exact left it as it was for good.
      uint8_t q = quality_[idx] == kRough ? kRough : kExact;
      const auto parts = paint_from_slot(c, c.rect, 0.25, q);
      if (parts.size() != 1 || parts[0] != c.rect) {
        for (const Rect& part : parts) {
          if (code_rect(c, part, hot[i])) q = kRough;
        }
        quality_[idx] = q;
        if (text && q != kExact) owe_lossless(idx);
        continue;
      }
    }
    const int how = code_rect(c, c.rect, i < n_changed && hot[i]);
    if (how == kDeferred) {
      quality_[idx] = kRough;
    } else if (how == kCodedVideo) {
      refine_after.push_back(i);
      quality_[idx] = kRough;
      // (A text mode keyframe's placeholder is in the picture, not the tile.)
      stale_[idx] = text && key && pending_lossless_[idx] ? kStaleText : 0;
    } else {
      // (A quick pass leaves it rough: it is sent again once the screen is
      // still, like a tile left at video quality.)
      quality_[idx] = last_coarse ? kRough : kExact;
      coarse_[idx] = last_coarse;
      stale_[idx] = 0;
    }
  }
  coarse_count_ = 0;
  for (uint8_t v : coarse_) coarse_count_ += v;
  // Text mode: a tile coded again is no longer taken from a picture that
  // was held back (if it is video again, it is listed anew below). Listing
  // it with a later picture would paint that picture's flattened tile over
  // text the viewer has received since.
  if (text && !carry_video_tiles_.empty()) {
    std::erase_if(carry_video_tiles_, [&](const tile::TileChange& t) { return recoded[tile_index(t)] != 0; });
  }
  // The viewer takes this frame's video tiles plus the ones still refining,
  // and those of a picture the encoder dropped.
  std::vector<tile::TileChange> region_tiles = video_tiles;
  if (!carry_video_tiles_.empty()) {
    // Every part of a dropped picture is listed again (a tile may have had
    // several, and this frame may code another part of it), once, unless
    // this frame lists its tile whole anyway.
    std::vector<uint8_t> whole(n_tiles, 0);
    std::set<std::tuple<int32_t, int32_t, int32_t, int32_t>> listed;
    for (const auto& t : region_tiles) {
      listed.insert({t.rect.x, t.rect.y, t.rect.w, t.rect.h});
      if (t.rect.w == kTileSize && t.rect.h == kTileSize) whole[tile_index(t)] = 1;
    }
    for (const auto& t : carry_video_tiles_) {
      if (whole[tile_index(t)]) continue;
      if (listed.insert({t.rect.x, t.rect.y, t.rect.w, t.rect.h}).second) region_tiles.push_back(t);
    }
    carry_video_tiles_.clear();
  }
  // A tile left in many video parts (the strips and rows a scroll leaves
  // between moved ones) is listed as one rect around them: each part costs
  // the region list ~6 bytes, and on a busy scroll at 0.4 Mbps frames
  // listed over a thousand of them (6.7 kB, four times the picture). The
  // pixels of the rect the encoder did not get yet (moved ones; lossless
  // ones are painted over the picture anyway) are given to it now, and the
  // tile is left rough, to be refreshed once static.
  {
    constexpr size_t kMaxPartsPerTile = 4;
    std::unordered_map<size_t, std::pair<size_t, Rect>> per_tile;  // parts, bounds
    for (const auto& t : region_tiles) {
      auto [it, fresh] = per_tile.try_emplace(tile_index(t), 0, t.rect);
      ++it->second.first;
      if (!fresh) {
        Rect& b = it->second.second;
        const int32_t x0 = std::min(b.x, t.rect.x), y0 = std::min(b.y, t.rect.y);
        const int32_t x1 = std::max(b.right(), t.rect.right()), y1 = std::max(b.y + b.h, t.rect.y + t.rect.h);
        b = {x0, y0, x1 - x0, y1 - y0};
      }
    }
    bool coarsened = false;
    for (auto& [idx, pb] : per_tile) {
      if (pb.first > kMaxPartsPerTile) {
        coarsened = true;
        break;
      }
    }
    if (coarsened) {
      std::vector<tile::TileChange> kept;
      kept.reserve(region_tiles.size());
      std::vector<uint8_t> done(n_tiles, 0);
      for (const auto& t : region_tiles) {
        const size_t idx = tile_index(t);
        const auto& pb = per_tile[idx];
        if (pb.first <= kMaxPartsPerTile) {
          kept.push_back(t);
          continue;
        }
        if (done[idx]) continue;
        done[idx] = 1;
        CopyRectInto(frame.bgra, pb.second, enc_input_.data(), enc_stride);
        // Lossless parts stay flattened, as everywhere else.
        for (const auto& lt : lossless_tiles) {
          if (lt.rect.intersects(pb.second)) FillRect(lt.rect, lt.colors[0], enc_input_.data(), enc_stride);
        }
        tile::TileChange whole = t;
        whole.rect = pb.second;
        kept.push_back(whole);
        quality_[idx] = kRough;
      }
      region_tiles = std::move(kept);
    }
  }
  if (refining_ > 0) {
    for (int32_t row = 0; row < differ_.rows(); ++row) {
      for (int32_t col = 0; col < differ_.cols(); ++col) {
        uint8_t& r = refine_[static_cast<size_t>(row) * differ_.cols() + col];
        if (r == 0) continue;
        Rect rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize};
        rect.w = std::min(rect.w, width_ - rect.x);
        rect.h = std::min(rect.h, height_ - rect.y);
        region_tiles.push_back({col, row, 0, 0, rect});
        if (--r == 0) {
          --refining_;
          // The encoder has had its chance to sharpen this tile.
          quality_[static_cast<size_t>(row) * differ_.cols() + col] = kRefined;
        }
      }
    }
  }
  // A progressive keyframe's picture is left as it is until the text is
  // done: refining it at the bitrate left over made it worse, not better,
  // and the intra picture at the end replaces it anyway.
  if (progressive_key) refine_after.clear();
  for (size_t i : refine_after) {
    set_refine(changes[i], refine_frames);
    // With no refinement planned, a refresh would not do better either.
    if (refine_frames == 0) quality_[tile_index(changes[i])] = kRefined;
  }
  // The end of a progressive keyframe: the text has gone out losslessly, so
  // the encoder's picture is now mostly flat and a fresh intra picture of it
  // spends its bits on what is left (photos, gradients), which the first
  // picture had to share with all the text. Only tiles where the encoder's
  // input still matches the screen (not moved ones) are taken from it.
  const bool sharpen = sharpen_video_ && !key;
  sharpen_video_ = false;
  if (sharpen) {
    // Flatten the text the progressive tiles left in the encoder's input
    // (where it is still there), so the intra picture does not spend on it.
    for (size_t idx = 0; idx < n_tiles; ++idx) {
      if (!unflattened_[idx]) continue;
      unflattened_[idx] = 0;
      if (quality_[idx] != kExact) continue;
      const auto col = static_cast<int32_t>(idx % differ_.cols());
      const auto row = static_cast<int32_t>(idx / differ_.cols());
      Rect rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize};
      rect.w = std::min(rect.w, width_ - rect.x);
      rect.h = std::min(rect.h, height_ - rect.y);
      bool same = true;
      for (int32_t y = 0; y < rect.h && same; ++y) {
        const size_t off = static_cast<size_t>(rect.y + y) * enc_stride + rect.x * 4;
        same = std::memcmp(enc_input_.data() + off, prev_.data() + off,
                           static_cast<size_t>(rect.w) * 4) == 0;
      }
      if (!same) continue;  // recoded since
      uint32_t px;
      std::memcpy(&px, enc_input_.data() + static_cast<size_t>(rect.y) * enc_stride + rect.x * 4, 4);
      FillRect(rect, px & 0x00FFFFFF, enc_input_.data(), enc_stride);
    }
    std::vector<uint8_t> listed(n_tiles, 0);
    for (const auto& t : region_tiles) listed[tile_index(t)] = 1;
    for (int32_t row = 0; row < differ_.rows(); ++row) {
      for (int32_t col = 0; col < differ_.cols(); ++col) {
        const size_t idx = static_cast<size_t>(row) * differ_.cols() + col;
        if (listed[idx] || quality_[idx] == kExact || regions_.RegionOf(col, row) >= 0) continue;
        Rect rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize};
        rect.w = std::min(rect.w, width_ - rect.x);
        rect.h = std::min(rect.h, height_ - rect.y);
        bool same = true;
        for (int32_t y = 0; y < rect.h && same; ++y) {
          const size_t off = static_cast<size_t>(rect.y + y) * enc_stride + rect.x * 4;
          same = std::memcmp(enc_input_.data() + off, prev_.data() + off,
                             static_cast<size_t>(rect.w) * 4) == 0;
        }
        if (!same) continue;
        tile::TileChange t{col, row, 0, 0, rect};
        region_tiles.push_back(t);
        set_refine(t, refine_frames);
        quality_[idx] = refine_frames == 0 ? kRefined : kRough;
      }
    }
  }
  // Still-screen polish: with the screen nearly still, the tiles left at
  // video quality are taken from the next pictures too, and those are coded
  // with the lower QP floor (HostOptions::still_refine_frames).
  const bool still = still_frames > 0 && quiet_ && !key && !sharpen;
  if (still && polish_owed_) {
    std::vector<uint8_t> listed(n_tiles, 0);
    for (const auto& t : region_tiles) listed[tile_index(t)] = 1;
    for (int32_t row = 0; row < differ_.rows(); ++row) {
      for (int32_t col = 0; col < differ_.cols(); ++col) {
        const size_t idx = static_cast<size_t>(row) * differ_.cols() + col;
        if (polish_[idx] == 0) continue;
        if (quality_[idx] == kExact) {
          polish_[idx] = 0;
          continue;
        }
        // Not yet: still in its ordinary refinement, about to be sent
        // losslessly, or part of a video area.
        if (listed[idx] || refine_[idx] > 0 || pending_lossless_[idx] || regions_.RegionOf(col, row) >= 0) continue;
        Rect rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize};
        rect.w = std::min(rect.w, width_ - rect.x);
        rect.h = std::min(rect.h, height_ - rect.y);
        if (polish_[idx] == kUnpolished) {
          if (frame_no_ - born_[idx] < kRefreshDelay) continue;  // may still be changing
          // Only what the encoder's picture really shows: a tile the viewer
          // got by a move or from the cache is not in it.
          bool same = true;
          for (int32_t y = 0; y < rect.h && same; ++y) {
            const size_t off = static_cast<size_t>(rect.y + y) * enc_stride + rect.x * 4;
            same = std::memcmp(enc_input_.data() + off, prev_.data() + off, static_cast<size_t>(rect.w) * 4) == 0;
          }
          if (!same) {
            polish_[idx] = 0;
            continue;
          }
          polish_[idx] = still_frames;
        }
        region_tiles.push_back({col, row, 0, 0, rect});
        if (--polish_[idx] == 0) quality_[idx] = kRefined;
      }
    }
  }
  const uint64_t t1 = capture::MonotonicMicros();

  std::optional<codec::HevcAccessUnit> au;
  // The viewer does not get these tiles' new content with this frame.
  auto mark_video_stale = [&] {
    if (!text) return;
    for (const auto& t : video_tiles) stale_[tile_index(t)] |= kStaleVideo;
  };
  if (key || !region_tiles.empty()) {
    encoder_->SetMinQp(still ? options_.still_min_qp : 0);
    // (Text mode ends a progressive keyframe without the intra picture: the
    // tiles left at the first picture's quality are refined by the next.)
    au = encoder_->Encode({enc_input_.data(), width_, height_, enc_stride}, frame.capture_time_us,
                          key || (sharpen && !text));
    if (au && text) {
      for (auto& st : stale_) st &= static_cast<uint8_t>(~kStaleVideo);
    }
    if (!au && !key && encoder_->last_error().empty()) {
      mark_video_stale();
      // The encoder chose to drop this picture (VideoToolbox does at low
      // rates). Its references are intact and its input already holds this
      // frame, so the next picture carries these changes: send the rest of
      // the frame as usual and list its video tiles with the next picture.
      // (A full refresh here made the next frame a keyframe, which made the
      // encoder drop more: at 1 Mbps most frames were lost that way.)
      carry_video_tiles_.insert(carry_video_tiles_.end(), region_tiles.begin(), region_tiles.end());
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.encoder_drops;
    } else if (!au) {
      // The encoder failed (or dropped a keyframe); make sure the next one
      // fully refreshes the viewer.
      force_keyframe_ = true;
      differ_.Reset();
      std::fill(refine_.begin(), refine_.end(), 0);
      refining_ = 0;
      std::fill(quality_.begin(), quality_.end(), kRough);
      // An empty error means the encoder chose to drop the frame, which is
      // not a failure.
      const std::string& why = encoder_->last_error();
      if (why.empty()) return true;
      if (error) *error = why;
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.encode_errors;
      return false;
    }
  }
  const uint64_t t2 = capture::MonotonicMicros();

  const uint64_t id = next_frame_id_++;
  std::vector<uint8_t> region_bytes;
  size_t region_payload = 0, region_pictures = 0;
  uint64_t region_us = 0;
  if (n_regions > 0 &&
      !EncodeRegions(frame, region_dirty, id, region_bytes, region_payload, region_pictures,
                     region_us)) {
    // Give the areas back to the normal path: their tiles get re-sent from
    // the current frame in the next one.
    for (const auto& reg : regions_.regions()) {
      for (size_t idx : TilesOf(reg.rect)) pending_sharpen_.push_back(idx);
    }
    regions_.Clear();
    region_enc_.clear();
    regions_disabled_ = true;
    region_bytes.clear();
    region_payload = region_pictures = 0;
  }
  if (use_regions) {
    for (const auto& t : video_tiles) busy[static_cast<size_t>(t.row) * differ_.cols() + t.col] = 1;
    FinishRegions(busy, region_changed);
  }

  // Deflate only after the video encode succeeded: the stream's history must
  // match what the viewer actually receives.
  const size_t n_lossless = lossless_tiles.size();
  const size_t n_loads = loads.size();
  codec::RegionOps ops;
  ops.stores = std::move(stores);
  for (const auto& m : moves) ops.moves.push_back({m.dst, m.dx, m.dy});
  ops.loads = std::move(loads);
  if (text && options_.context_tiles) {
    for (auto& t : lossless_tiles) {
      const size_t n = t.colors.size();
      const size_t most = options_.context_tiles_64 ? 64 : 16;
      (n >= 2 && n <= most ? ops.context_tiles : ops.tiles).push_back(std::move(t));
    }
  } else {
    ops.tiles = std::move(lossless_tiles);
  }
  std::vector<uint8_t> lossless_data, context_data;
  if (!ops.empty()) lossless_data = lossless_.Encode(ops, key, &context_data);
  const size_t lossless_size = lossless_data.size() + context_data.size();
  if (progressive_active_ && !key && pending_lossless_count_ == 0) {
    progressive_active_ = false;
    // Next frame, if any tile is left at the first picture's quality.
    sharpen_video_ = std::find_if(quality_.begin(), quality_.end(),
                                  [](uint8_t q) { return q != kExact; }) != quality_.end();
  }
  if (progressive_sent > 0 && n_lossless > 0) {
    lossless_bytes_per_tile_ = 0.7 * lossless_bytes_per_tile_ +
                               0.3 * static_cast<double>(lossless_size) / static_cast<double>(n_lossless);
  }
  if (n_lossless > 0 && lossless_raw > 256) {
    lossless_ratio_ = std::clamp(
        0.8 * lossless_ratio_ + 0.2 * static_cast<double>(lossless_size) / lossless_raw, 0.05, 1.0);
  }
  const uint64_t t3 = capture::MonotonicMicros();
  if (text && !au && region_bytes.empty() && lossless_data.empty()) {
    // Everything this frame changed waits (for the lossless budget, or for
    // the next video picture): no frame goes out. At these rates even an
    // empty one a frame interval adds up.
    --next_frame_id_;
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.frames_skipped;
    return true;
  }

  std::vector<uint8_t> bytes;
  proto::EncodeTo(proto::FrameBegin{id, frame.capture_time_us}, bytes);
  size_t video_bytes = 0;
  if (au) {
    proto::VideoData vd;
    vd.frame_id = id;
    vd.keyframe = au->keyframe;
    vd.parameter_sets = std::move(au->parameter_sets);
    vd.nal_data = std::move(au->nal_data);
    // Only a requested keyframe covers the whole screen: the viewer must not
    // take flattened lossless tiles from any other picture.
    if (!key) vd.regions = TilesToRegions(region_tiles);
    video_bytes = vd.nal_data.size();
    proto::EncodeTo(vd, bytes);
  }
  // Area pictures go on top of the full-screen one.
  bytes.insert(bytes.end(), region_bytes.begin(), region_bytes.end());
  if (!lossless_data.empty()) {
    proto::EncodeTo(proto::LosslessData{id, key, lossless_data, context_data}, bytes);
  }
  proto::EncodeTo(proto::FrameEnd{id}, bytes);
  transport_->Send(transport::Channel::kVideo, bytes);

  std::lock_guard<std::mutex> lock(mu_);
  ++stats_.frames_encoded;
  if (au) {
    ++stats_.video_frames;
    if (au->keyframe) ++stats_.keyframes;
    stats_.encode_us_total += t2 - t1;
  }
  stats_.bytes_sent += bytes.size();
  stats_.video_bytes += video_bytes + region_payload;
  stats_.region_bytes += region_payload;
  stats_.region_pictures += region_pictures;
  stats_.region_encode_us_total += region_us;
  stats_.lossless_bytes += lossless_size;
  lossless_tokens_ -= static_cast<double>(lossless_size);
  // (That keyframe is not held against the frames after it.)
  if (text && key) lossless_tokens_ = std::max(lossless_tokens_, 0.0);
  stats_.lossless_tiles += n_lossless;
  stats_.moved_tiles += moved_tiles;
  moving_ = moved_tiles > 0 && over_lossless_budget;
  stats_.cached_tiles += n_loads;
  stats_.refreshed_tiles += refresh.size() - progressive_sent;
  stats_.progressive_tiles += progressive_sent;
  stats_.lossless_us_total += (t1 - t0) + (t3 - t2);
  return true;
}

std::vector<size_t> HostSession::TilesOf(const Rect& r) const {
  std::vector<size_t> out;
  for (int32_t row = r.y / kTileSize; row * kTileSize < r.bottom() && row < differ_.rows(); ++row) {
    for (int32_t col = r.x / kTileSize; col * kTileSize < r.right() && col < differ_.cols(); ++col) {
      out.push_back(static_cast<size_t>(row) * differ_.cols() + col);
    }
  }
  return out;
}

int64_t HostSession::RegionBitrate(const Rect& r) const {
  const double share = 2.0 * r.w * r.h / (static_cast<double>(width_) * height_);
  int64_t bps = static_cast<int64_t>(static_cast<double>(options_.bitrate_bps) * std::clamp(share, 0.25, 1.0));
  // A fixed area bitrate still has to fit the (adapted) budget.
  if (options_.region_bitrate_bps > 0) bps = std::min(options_.region_bitrate_bps, options_.bitrate_bps);
  return bps;
}

void HostSession::SetTextMode(bool on) {
  if (on == options_.text_mode) return;
  options_.text_mode = on;
  // Exact colours again, and nothing stale: a full refresh.
  if (!on) force_keyframe_ = true;
}

void HostSession::SetBitrate(int64_t bps) {
  options_.bitrate_bps = bps;
  // (Text mode as well: its video is coded as without it. A fixed video
  // allowance of 1 Mbps made a full-screen transition a second of blocks.)
  encoder_->SetBitrate(bps);
  for (auto& [stream, re] : region_enc_) {
    for (const auto& reg : regions_.regions()) {
      if (reg.stream == stream && re.enc) re.enc->SetBitrate(RegionBitrate(reg.rect));
    }
  }
}

bool HostSession::EncodeRegions(const capture::CapturedFrame& frame, const std::vector<bool>& dirty,
                                uint64_t frame_id, std::vector<uint8_t>& out, size_t& bytes,
                                size_t& pictures, uint64_t& us) {
  const auto& regs = regions_.regions();
  for (size_t k = 0; k < regs.size(); ++k) {
    const tile::VideoRegion& reg = regs[k];
    RegionEncoder& re = region_enc_[reg.stream];
    if (!re.enc) {
      codec::EncoderConfig cfg;
      cfg.width = reg.rect.w;
      cfg.height = reg.rect.h;
      cfg.fps = options_.fps;
      cfg.min_qp = options_.region_min_qp;
      cfg.bitrate_bps = RegionBitrate(reg.rect);
      std::string err;
      re.enc = options_.region_encoder_factory(cfg, &err);
      if (!re.enc) return false;
      re.need_key = true;
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.regions_opened;
    }
    if (!dirty[k] && !re.need_key) continue;
    const tile::ImageView crop{frame.bgra.data + static_cast<size_t>(reg.rect.y) * frame.bgra.stride +
                                   static_cast<size_t>(reg.rect.x) * 4,
                               reg.rect.w, reg.rect.h, frame.bgra.stride};
    const uint64_t t0 = capture::MonotonicMicros();
    auto au = re.enc->Encode(crop, frame.capture_time_us, re.need_key);
    us += capture::MonotonicMicros() - t0;
    if (!au) {
      if (!re.enc->last_error().empty()) return false;
      // Dropped by the encoder (VideoToolbox does at low rates). Its
      // references are intact and every picture codes the whole area, so the
      // next one catches up; a dropped keyframe is asked for again. (Asking
      // for a keyframe after every drop made the encoder drop more.)
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.encoder_drops;
      continue;
    }
    re.need_key = false;
    proto::VideoData vd;
    vd.frame_id = frame_id;
    vd.keyframe = au->keyframe;
    vd.parameter_sets = std::move(au->parameter_sets);
    vd.nal_data = std::move(au->nal_data);
    vd.stream = reg.stream;
    vd.x = reg.rect.x;
    vd.y = reg.rect.y;
    bytes += vd.nal_data.size();
    ++pictures;
    proto::EncodeTo(vd, out);
  }
  return true;
}

void HostSession::FinishRegions(const std::vector<uint8_t>& busy,
                                const std::vector<uint8_t>& changed) {
  const auto closed = regions_.Update(busy, changed);
  for (const auto& reg : closed) {
    for (size_t idx : TilesOf(reg.rect)) pending_sharpen_.push_back(idx);
  }
  // Encoders of areas that closed or were replaced by a larger one.
  std::erase_if(region_enc_, [&](const auto& kv) {
    for (const auto& reg : regions_.regions()) {
      if (reg.stream == kv.first) return false;
    }
    return true;
  });
  if (!closed.empty()) {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.regions_closed += closed.size();
  }
}

void HostSession::OnReceive(transport::Channel ch, std::span<const uint8_t> bytes) {
  proto::StreamParser* parser = nullptr;
  if (ch == transport::Channel::kInput) {
    parser = &input_parser_;
  } else if (ch == transport::Channel::kControl) {
    parser = &control_parser_;
  } else {
    return;
  }
  parser->Append(bytes);
  while (auto m = parser->Next()) {
    if (std::holds_alternative<proto::KeyframeRequest>(*m)) {
      force_keyframe_ = true;
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.keyframe_requests;
      continue;
    }
    if (auto* in = std::get_if<proto::InputEvent>(&*m)) {
      InputFn fn;
      {
        std::lock_guard<std::mutex> lock(mu_);
        fn = on_input_;
      }
      if (fn) fn(*in);
    }
  }
}

}  // namespace rdacore::session
