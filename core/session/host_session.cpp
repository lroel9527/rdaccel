#include "session/host_session.h"

#include <algorithm>
#include <cstring>

namespace rdaccel::session {

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
  return out;
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
      prev_(enc_input_.size(), 0) {
  transport_->SetReceiver(
      [this](transport::Channel ch, std::span<const uint8_t> bytes) { OnReceive(ch, bytes); });
  Send(transport::Channel::kControl,
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
  if (refine_.size() != n_tiles) {
    refine_.assign(n_tiles, 0);
    refining_ = 0;
    born_.assign(n_tiles, 0);
    quality_.assign(n_tiles, kRough);
    tile_slot_.assign(n_tiles, -1);
    tile_slot_hash_.assign(n_tiles, 0);
  }
  ++frame_no_;
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
  if (changes.empty() && !key && refining_ == 0 && refresh.empty()) {
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
      if ((stable || recurring) && !cache_.Find(c.prev_hash)) {
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
  std::vector<bool> hot(changes.size());
  for (size_t i = 0; i < changes.size(); ++i) {
    uint8_t& n = seen_[changes[i].hash];
    if (n < 255) ++n;
    uint64_t& born = born_[tile_index(changes[i])];
    hot[i] = !key && frame_no_ - born <= 1;
    born = frame_no_;
  }

  std::vector<tile::Move> moves;
  std::vector<bool> moved;
  std::vector<std::vector<Rect>> residual;
  if (!key && options_.detect_moves) {
    moves = tile::DetectMoves({prev_.data(), width_, height_, enc_stride}, frame.bgra, changes,
                              moved, residual);
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
  std::vector<uint8_t> move_quality;
  for (const auto& m : moves) move_quality.push_back(quality_under(m.src()));
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
  };
  // Codes `r` (all or part of tile `c`) losslessly if it has few colours,
  // else through the video encoder. Returns true for video.
  auto code_rect = [&](const tile::TileChange& c, const Rect& r, bool is_hot) {
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
    if (options_.lossless && codec::ExtractPalette(frame.bgra, r, max_colors, t)) {
      FillRect(r, t.colors[0], enc_input_.data(), enc_stride);
      lossless_tiles.push_back(std::move(t));
      return false;
    }
    CopyRectInto(frame.bgra, r, enc_input_.data(), enc_stride);
    tile::TileChange part = c;
    part.rect = r;
    video_tiles.push_back(part);
    return true;
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
  std::vector<size_t> refine_after;  // tiles that went to video whole
  for (size_t i = 0; i < changes.size(); ++i) {
    const auto& c = changes[i];
    const size_t idx = tile_index(c);
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
    if (i < n_changed && !moved.empty() && moved[i]) {
      ++moved_tiles;
      uint8_t q = moved_in_quality(c.rect);
      if (!residual[i].empty()) q = std::min(q, quality_[idx]);  // rows that stayed put
      // Only the rows that did not move or stay put are left to code, and of
      // those, what a dragged window just uncovered can come from the cache.
      for (const Rect& r : residual[i]) {
        for (const Rect& part : paint_from_slot(c, r, 0.0, q)) {
          if (code_rect(c, part, hot[i])) q = kRough;
        }
      }
      quality_[idx] = q;
      continue;
    }
    // Tile cache, part 2: content the viewer has kept. Like moves, the
    // encoder's picture is left alone here.
    if (i < n_changed && !key && options_.tile_cache) {
      if (auto slot = cache_.Find(c.hash)) {
        loads.push_back({*slot, c.rect});
        quality_[idx] = slot_quality_[*slot];
        continue;
      }
    }
    // A tile that was static and now changes partly (typing into a line
    // that was stored, a menu closing) may get most of its pixels from the
    // slot; hot tiles (video, animation) are left whole.
    if (i < n_changed && !hot[i]) {
      uint8_t q = kExact;
      const auto parts = paint_from_slot(c, c.rect, 0.25, q);
      if (parts.size() != 1 || parts[0] != c.rect) {
        for (const Rect& part : parts) {
          if (code_rect(c, part, hot[i])) q = kRough;
        }
        quality_[idx] = q;
        continue;
      }
    }
    if (code_rect(c, c.rect, i < n_changed && hot[i])) {
      refine_after.push_back(i);
      quality_[idx] = kRough;
    } else {
      quality_[idx] = kExact;
    }
  }
  // The viewer takes this frame's video tiles plus the ones still refining.
  std::vector<tile::TileChange> region_tiles = video_tiles;
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
  for (size_t i : refine_after) {
    set_refine(changes[i], refine_frames);
    // With no refinement planned, a refresh would not do better either.
    if (refine_frames == 0) quality_[tile_index(changes[i])] = kRefined;
  }
  const uint64_t t1 = capture::MonotonicMicros();

  std::optional<codec::HevcAccessUnit> au;
  if (key || !region_tiles.empty()) {
    au = encoder_->Encode({enc_input_.data(), width_, height_, enc_stride}, frame.capture_time_us,
                          key);
    if (!au) {
      // The encoder dropped or failed this frame; make sure the next one fully
      // refreshes the viewer.
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

  // Deflate only after the video encode succeeded: the stream's history must
  // match what the viewer actually receives.
  const size_t n_lossless = lossless_tiles.size();
  const size_t n_loads = loads.size();
  codec::RegionOps ops;
  ops.stores = std::move(stores);
  for (const auto& m : moves) ops.moves.push_back({m.dst, m.dx, m.dy});
  ops.loads = std::move(loads);
  ops.tiles = std::move(lossless_tiles);
  std::vector<uint8_t> lossless_data;
  if (!ops.empty()) lossless_data = lossless_.Encode(ops, key);
  const uint64_t t3 = capture::MonotonicMicros();

  const uint64_t id = next_frame_id_++;
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
  if (!lossless_data.empty()) {
    proto::EncodeTo(proto::LosslessData{id, key, lossless_data}, bytes);
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
  stats_.video_bytes += video_bytes;
  stats_.lossless_bytes += lossless_data.size();
  stats_.lossless_tiles += n_lossless;
  stats_.moved_tiles += moved_tiles;
  stats_.cached_tiles += n_loads;
  stats_.refreshed_tiles += refresh.size();
  stats_.lossless_us_total += (t1 - t0) + (t3 - t2);
  return true;
}

void HostSession::OnReceive(transport::Channel ch, std::span<const uint8_t> bytes) {
  if (ch != transport::Channel::kInput && ch != transport::Channel::kControl) return;
  // Both channels share one parser here: the viewer only sends whole
  // messages, so chunks never interleave mid-message on a single transport.
  input_parser_.Append(bytes);
  while (auto m = input_parser_.Next()) {
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

}  // namespace rdaccel::session
