#include "session/host_session.h"

#include "tile/text_cell.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
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
      picture_(width, height),
      prev_(static_cast<size_t>(width) * height * 4, 0),
      regions_(options.region_params),
      next_frame_id_(options.first_frame_id) {
  if (options_.own_receiver) {
    transport_->SetReceiver(
        [this](transport::Channel ch, std::span<const uint8_t> bytes) { OnReceive(ch, bytes); });
  }
  if (options_.text_mode) SetBitrate(options_.bitrate_bps);  // the encoder's share
  if (options_.mirror_decoder_factory && options_.text_video_min_db > 0) {
    if (auto decoder = options_.mirror_decoder_factory()) mirror_ = std::make_unique<PictureMirror>(std::move(decoder));
  }
  SendHello();
}

void HostSession::SendHello() {
  Send(options_.hello_on_video ? transport::Channel::kVideo : transport::Channel::kControl,
       proto::Hello{proto::kProtocolVersion, static_cast<uint32_t>(width_),
                    static_cast<uint32_t>(height_)});
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

bool Overlap(const Rect& a, const Rect& b) {
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

}  // namespace

// What one frame works with on its way through OnFrame's stages.
struct HostSession::Frame {
  explicit Frame(const capture::CapturedFrame& captured) : in(captured) {}

  const capture::CapturedFrame& in;
  bool key = false;              // a keyframe: every tile coded afresh
  bool progressive_key = false;  // ... going out video first, lossless after
  bool text = false;             // text mode
  bool use_regions = false;      // video areas get streams of their own
  size_t n_tiles = 0;
  // The tiles that changed; from CodeTiles on, followed by the ones coded
  // again although they did not change (`refresh`).
  std::vector<tile::TileChange> changes;
  size_t n_changed = 0;

  // Video areas (TrackAreas).
  size_t n_regions = 0;
  std::vector<bool> region_dirty;            // per area: changed this frame
  std::vector<uint8_t> busy, region_changed;  // per tile, for the area tracker
  bool region_needs_key = false;
  bool regions_to_send = false;

  // Work owed from earlier frames (PlanOwedWork, PlanPolish).
  bool coarse_now = false;               // a screenful owed: tiles get their quick pass first
  std::vector<tile::TileChange> retakes;  // poor video taken again (PlanRetakes)
  std::vector<size_t> refresh;           // tiles to code again
  std::vector<uint8_t> progressive_now;  // per tile: its turn of the owed queue now
  size_t progressive_sent = 0;
  double owed_budget = 0;  // what the owed tiles of this frame may come to
  double owed_spent = 0;   // ... and what the ones coded so far are counted at
  int refine_option = 0;   // refinement pictures (text mode: none)
  uint8_t still_frames = 0;
  uint8_t refine_frames = 0;

  // Changes (ClassifyChanges) and motion (DetectMotion).
  std::vector<bool> hot;  // per change: changed in the frame before too
  bool mass_motion = false;
  std::vector<tile::Move> moves;
  std::vector<bool> moved;                     // per change: covered by moves, fully or partly
  std::vector<std::vector<Rect>> residual;     // per change: the parts the moves leave to code
  std::vector<uint8_t> unmoved;                // per tile: its moves dropped (text mode)
  std::vector<uint8_t> move_quality;           // per move: the worst tile it copies from
  std::vector<uint8_t> move_tainted;           // per move: copies from a stale tile
  bool any_tainted = false;

  // What the frame sends (CodeTiles).
  std::vector<codec::SlotOp> stores, loads;      // tile cache
  size_t moved_tiles = 0;
  std::vector<codec::PaletteTile> lossless_tiles;
  std::vector<uint8_t> lossless_px;              // per lossless tile: the pixels' quality
  std::vector<tile::TileChange> video_tiles;     // video coded, whole or in part
  double lossless_budget = 0, lossless_spent = 0, lossless_raw = 0;
  bool over_lossless_budget = false;
  bool last_coarse = false;            // the last rect went out at the quick pass's colours
  std::vector<uint8_t> prev_tile;      // the previous frame's pixels of the tile being coded
  std::vector<size_t> refine_after;    // changes that went to video whole
  std::vector<uint8_t> recoded;        // per tile, text mode: coded this frame
  size_t put_back = 0;                 // owed tiles left for the next frame

  // The picture (AssemblePicture, EncodePicture).
  std::vector<tile::TileChange> region_tiles;  // what the viewer takes from it
  bool sharpen = false;  // the picture that ends a progressive keyframe
  bool still = false;    // coded with the still-screen QP floor
  std::optional<codec::HevcAccessUnit> au;
  bool picture_coded = false;                     // the full-screen encoder made a picture
  std::optional<codec::HevcAccessUnit> mirrored;  // ... kept for the mirror (text mode)

  FrameTrace trace;  // filled only for HostOptions::trace
  bool tracing = false;
  uint64_t t0 = 0, t1 = 0, t2 = 0;  // stage times, for the statistics

  // What the moves copy into `tile_rect`: as good as the worst tile they
  // come from, and whether any comes from a stale one.
  uint8_t MovedInQuality(const Rect& tile_rect) const {
    uint8_t q = kExact;
    for (size_t k = 0; k < moves.size(); ++k) {
      if (Overlap(moves[k].dst, tile_rect)) q = std::min(q, move_quality[k]);
    }
    return q;
  }
  bool MovedInTainted(const Rect& tile_rect) const {
    if (!any_tainted) return false;
    for (size_t k = 0; k < moves.size(); ++k) {
      if (move_tainted[k] && Overlap(moves[k].dst, tile_rect)) return true;
    }
    return false;
  }
};

// A frame goes through these stages. Most frames of a still screen stop
// after the planning: nothing changed and nothing is owed.
bool HostSession::OnFrame(const capture::CapturedFrame& in, std::string* error) {
  Frame f(in);
  f.tracing = static_cast<bool>(options_.trace);
  BeginFrame(f);
  TrackAreas(f);
  FillLosslessBucket(f);
  PlanOwedWork(f);
  PlanPolish(f);
  PlanRetakes(f);
  if (NothingToDo(f)) {
    quiet_ = true;
    moving_ = false;
    if (f.use_regions) FinishRegions(f.busy, f.region_changed, in.capture_time_us);
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.frames_skipped;
    return true;
  }
  f.t0 = capture::MonotonicMicros();
  StoreLeavingTiles(f);
  ClassifyChanges(f);
  DetectMotion(f);
  CodeTiles(f);
  AssemblePicture(f);
  f.t1 = capture::MonotonicMicros();
  bool done = false;
  const bool ok = EncodePicture(f, error, &done);
  if (done) return ok;
  f.trace.picture_bytes = f.au ? f.au->nal_data.size() : 0;
  f.t2 = capture::MonotonicMicros();
  SendFrame(f);
  MeasureVideo(f);
  if (f.tracing) {
    f.trace.frame_no = frame_no_;
    f.trace.mass_motion = f.mass_motion;
    f.trace.key = f.key;
    f.trace.still = f.still;
    f.trace.sharpen = f.sharpen;
    f.trace.video_rects = static_cast<int>(f.video_tiles.size());
    options_.trace(f.trace);
  }
  return true;
}

// The tiles that changed; the state a keyframe or a new size starts over.
void HostSession::BeginFrame(Frame& f) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.frames_captured;
  }
  f.key = force_keyframe_.exchange(false);
  // A keyframe classifies every tile, so the encoder input is rebuilt fully.
  if (f.key) differ_.Reset();
  f.changes = differ_.Diff(f.in.bgra, f.key ? std::span<const Rect>() : f.in.dirty);
  f.n_tiles = static_cast<size_t>(differ_.cols()) * differ_.rows();
  if (f.key) carry_video_tiles_.clear();  // the whole screen goes anyway
  if (ledger_.size() != f.n_tiles) {
    carry_video_tiles_.clear();
    polish_owed_ = false;
    ledger_.Resize(width_, height_, differ_.cols(), differ_.rows());
  }
  // A keyframe starts the progressive sequence over.
  f.progressive_key = f.key && options_.progressive && options_.lossless;
  if (f.key) {
    ledger_.StartKeyframe();
    progressive_active_ = f.progressive_key;
    sharpen_video_ = false;
    progressive_last_us_ = f.in.capture_time_us;
  }
  ++frame_no_;
  f.text = options_.text_mode && options_.lossless;
  // (Video areas as without text mode: a film in its own cropped stream is
  // both smoother and cheaper than in full-screen pictures.)
  f.use_regions = options_.region_encoder_factory && !regions_disabled_;
  if (regions_.cols() != differ_.cols() || regions_.rows() != differ_.rows()) {
    regions_.Reset(width_, height_);
  }
  if (f.key) {
    regions_.Clear();
    region_enc_.clear();
    pending_sharpen_.clear();
  }
}

// Video areas: their tiles leave the normal path and go to the area's own
// stream (see EncodeRegions).
void HostSession::TrackAreas(Frame& f) {
  f.n_regions = regions_.regions().size();
  f.region_dirty.assign(f.n_regions, false);
  f.busy.assign(f.use_regions ? f.n_tiles : 0, 0);
  f.region_changed.assign(f.use_regions ? f.n_tiles : 0, 0);
  for (size_t k = 0; k < f.n_regions; ++k) {
    const auto it = region_enc_.find(regions_.regions()[k].stream);
    if (it == region_enc_.end() || it->second.need_key) f.region_needs_key = true;
    // The viewer must never take these tiles from the full-screen picture,
    // which no longer follows them.
    for (size_t idx : TilesOf(regions_.regions()[k].rect)) ledger_.SetRefine(idx, 0);
  }
  if (f.n_regions > 0) {
    std::erase_if(f.changes, [&](const tile::TileChange& c) {
      const int k = regions_.RegionOf(c.col, c.row);
      if (k < 0) return false;
      const size_t idx = TileIndex(c);
      f.region_dirty[static_cast<size_t>(k)] = true;
      f.busy[idx] = f.region_changed[idx] = 1;
      CopyRectInto(f.in.bgra, c.rect, prev_.data(), width_ * 4);
      ledger_.EnterArea(idx, frame_no_, f.in.capture_time_us);
      return true;
    });
  }
  f.regions_to_send =
      f.region_needs_key || std::find(f.region_dirty.begin(), f.region_dirty.end(), true) != f.region_dirty.end();
}

// The lossless bucket fills with time, also over frames that end up
// skipped below: owed tiles are picked by what it holds, so a still screen
// must keep filling it or they would wait for the next change.
void HostSession::FillLosslessBucket(Frame& f) {
  const double frame_us = 1e6 / std::max(1, options_.fps);
  // Nothing changed at all (a still screen, typically the Host re-sending
  // the last frame every 100 ms because something is owed): the video
  // sends nothing, so the owed tiles may use most of the link, and the
  // bucket holds one re-send's worth.
  const bool still = f.changes.empty() && !f.key && !holding_back_;
  // Text mode: 150 ms of it, so that what a key press changes goes out
  // at once after a pause (at 100 kbps three frame intervals hold 1.2 kB,
  // less than one tile of text).
  const double burst_us = f.text ? std::max(kLosslessBurstFrames * frame_us, 300'000.0)
                          : still ? std::max(kLosslessBurstFrames * frame_us, 100'000.0)
                                  : kLosslessBurstFrames * frame_us;
  // Owed tiles (a progressive keyframe, or tiles the budget sent as video)
  // get a larger share while the screen is nearly still, unless frames are
  // being held back: then the path is full already. While the screen keeps
  // moving (a scroll, a drag), lossless gets less, so what is uncovered goes
  // as video first and turns sharp once the screen stops: at 0.4 Mbps a
  // scroll's new rows of text alone need more than the link, and showing
  // every frame beats showing them exact while they fly past.
  const double share = f.text ? kTextLosslessShare
                       : moving_ ? kLosslessMovingShare
                       : (progressive_active_ || still) && !holding_back_ ? kProgressiveShare
                                                                          : kLosslessRateShare;
  bucket_.Fill(f.in.capture_time_us, options_.bitrate_bps, share, burst_us);
}

// Tiles coded again although they did not change: what the ledger has
// planned for them, as far as this frame's budget goes.
void HostSession::PlanOwedWork(Frame& f) {
  const size_t n_tiles = f.n_tiles;
  // Text mode: a screenful is owed, so tiles get their quick pass first.
  f.coarse_now = f.text && ledger_.owed_count() > kTextCoarseBacklog &&
                 static_cast<double>(ledger_.owed_count()) * std::max(lossless_bytes_per_tile_, 64.0) >
                     kTextCoarseBacklogSeconds * static_cast<double>(options_.bitrate_bps) / 8.0 * kTextLosslessShare;
  // Tiles the viewer shows at first-pass video quality (or copies of such)
  // that have been static for a while get sent again properly.
  if (!f.key && options_.refresh) {
    for (size_t i = 0; i < n_tiles && f.refresh.size() < kMaxRefreshPerFrame; ++i) {
      if (ledger_.DueRefresh(i, frame_no_)) f.refresh.push_back(i);
    }
  }
  // Palette content the encoder has finished with (see
  // TileState::palette_checked): owed losslessly, like the tiles below.
  if (!f.key && (quiet_ || f.text) && options_.lossless) {
    std::vector<uint8_t> listed(n_tiles, 0);
    for (const auto& c : f.changes) listed[TileIndex(c)] = 1;
    for (size_t i = 0; i < n_tiles; ++i) {
      if (!ledger_.DuePaletteCheck(i, frame_no_) || listed[i] || InArea(i)) continue;
      TileState& t = ledger_[i];
      t.palette_checked = t.born + 1;
      const Rect rect = TileRect(i);
      // As the tile would be coded on its turn (see CodeRectAs).
      codec::PaletteTile p;
      const bool palette =
          f.text ? codec::ExtractReducedPalette(f.in.bgra, rect, options_.text_colors, options_.lossless_max_colors,
                                                4096, p, nullptr, /*faithful=*/true)
                 : codec::ExtractPalette(f.in.bgra, rect, options_.lossless_max_colors, p);
      if (palette) ledger_.Owe(i);
    }
  }
  // Progressive keyframe: the next lossless tiles, as many as fit about
  // half of this frame's share of the bitrate.
  f.progressive_now.assign(n_tiles, 0);
  // Once the screen is nearly still, every tile left at video quality (sent
  // as video for want of budget, moved or copied from such a tile, or the
  // first picture's) is owed too, and goes out losslessly within the bucket
  // like the rest; one that is not palette content goes as video and is
  // refined by the encoder instead. The user's choice for a slow link:
  // blurry while it moves, sharp once it stops.
  // (Text mode does not wait for a still screen: nothing there is taken
  // from the video meanwhile, and a clock or a spinner somewhere must not
  // keep the rest at its quick pass.)
  if (!f.key && (quiet_ || f.text) && options_.lossless) {
    for (size_t i = 0; i < n_tiles; ++i) {
      if (ledger_.DueOwing(i, frame_no_) && !InArea(i)) ledger_.Owe(i);
    }
  }
  // Tiles the budget sent as video wait for a nearly still screen (they
  // would take from the video otherwise); a progressive keyframe goes on.
  // In text mode they were not sent as video, so they need not wait.
  if (!f.key && ledger_.owed_count() > 0 && (progressive_active_ || quiet_ || f.text)) {
    std::vector<uint8_t> listed(n_tiles, 0);
    for (const auto& c : f.changes) listed[TileIndex(c)] = 1;
    for (size_t idx : f.refresh) listed[idx] = 1;
    // By time rather than per frame: frames the transport made us skip
    // leave their share to the next one.
    const double frame_us = 1e6 / std::max(1, options_.fps);
    // At most kOwedMaxUs of it in one frame: a frame is on its way for as
    // long as its bytes take, and nothing else arrives meanwhile. (Six
    // frame intervals at 30 Hz were 200 ms; with the picture that refines
    // the video tiles in the same frame, 30 kB at 0.4 Mbps: most of a
    // second each time a scroll stopped.)
    const double dt_us = std::clamp(
        static_cast<double>(f.in.capture_time_us - std::min(f.in.capture_time_us, progressive_last_us_)),
        frame_us, std::max(frame_us, std::min(6 * frame_us, kOwedMaxUs)));
    const double budget = static_cast<double>(options_.bitrate_bps) / 8.0 * dt_us / 1e6 * kProgressiveShare;
    const double tokens = bucket_.tokens();
    f.owed_budget = std::min(budget, std::max(0.0, tokens));
    // Within the lossless bucket too: a progressive keyframe right after a
    // drop would otherwise send its tiles faster than the path takes them.
    const double per_tile = std::max(lossless_bytes_per_tile_, 64.0);
    const double fits = std::min(budget, std::max(0.0, tokens)) / per_tile;
    // At least one once the bucket holds a tile's worth, or the frame's
    // share, or is full: on a slow link one tile of detailed text can cost
    // more than the bucket holds, and waiting for more would wait forever.
    const bool one = tokens >= per_tile || tokens >= budget || tokens >= 0.999 * bucket_.cap();
    const auto n = static_cast<size_t>(std::clamp(fits, one ? 1.0 : 0.0, static_cast<double>(kMaxProgressivePerFrame)));
    const size_t start = ledger_.cursor;
    for (size_t k = 0; k < n_tiles && f.progressive_sent < n && ledger_.owed_count() > 0; ++k) {
      const size_t idx = (start + k) % n_tiles;
      if (!ledger_[idx].owed || f.progressive_now[idx]) continue;
      if (listed[idx]) continue;  // changed anyway: coded normally below
      if (InArea(idx)) {
        // Became video: it will never be lossless.
        ledger_.Settle(idx);
        continue;
      }
      f.refresh.push_back(idx);
      f.progressive_now[idx] = 1;
      ++f.progressive_sent;
      ledger_.cursor = idx + 1;
    }
    if (f.progressive_sent > 0) progressive_last_us_ = f.in.capture_time_us;
  }
  // Tiles of areas that just closed get one more, sharp, picture.
  if (!pending_sharpen_.empty()) {
    std::vector<uint8_t> listed(n_tiles, 0);
    for (const auto& c : f.changes) listed[TileIndex(c)] = 1;
    for (size_t idx : f.refresh) listed[idx] = 1;
    for (size_t idx : pending_sharpen_) {
      if (idx < n_tiles && !listed[idx] && !InArea(idx)) {
        listed[idx] = 1;
        f.refresh.push_back(idx);
      }
    }
    pending_sharpen_.clear();
  }
}

// Tiles at video quality that the still-screen polish has yet to do.
// (Text mode does not refine or polish, see HostOptions::text_refine: the
// pictures that brought photos, icons and the wallpaper to the normal
// mode's quality were half of its traffic while typing.)
void HostSession::PlanPolish(Frame& f) {
  f.refine_option = f.text && !options_.text_refine ? 0 : options_.refine_frames;
  f.still_frames = static_cast<uint8_t>(
      options_.still_min_qp > 0 && f.refine_option > 0
          ? std::clamp(options_.still_refine_frames, 0, static_cast<int>(kUnpolished) - 1)
          : 0);
  for (const auto& c : f.changes) ledger_[TileIndex(c)].polish = kUnpolished;
  polish_owed_ = false;
  if (f.still_frames > 0 && !f.key) {
    for (size_t i = 0; i < f.n_tiles && !polish_owed_; ++i) {
      TileState& t = ledger_[i];
      if (t.polish == 0) continue;
      if (t.quality == kExact) {
        t.polish = 0;
        continue;
      }
      polish_owed_ = !InArea(i);  // areas have encoders of their own
    }
  }
}

// Poor video (see MeasureVideo) is taken again from the next picture, cell
// by cell, once its tile has stood still, the screen is nearly still and the
// encoder has rested (kRetakeRestUs): it then has the bits to refine what it
// coded coarsely in a busy picture. Only the poor cells are listed; the text
// beside them, which the viewer has exactly, is not sent again.
void HostSession::PlanRetakes(Frame& f) {
  if (!ledger_.poor_possible()) return;
  if (!f.text || !mirror_ || !mirror_->in_step()) {
    ledger_.GiveUpPoor();  // nothing measures the pictures any more
    return;
  }
  if (f.key) return;  // every tile is coded afresh
  const bool now = f.changes.size() * 50 <= f.n_tiles &&
                   f.in.capture_time_us - std::min(f.in.capture_time_us, last_picture_us_) >= kRetakeRestUs;
  std::vector<uint8_t> busy(f.n_tiles, 0);  // coded in this frame anyway
  for (const auto& c : f.changes) busy[TileIndex(c)] = 1;
  for (size_t idx : f.refresh) busy[idx] = 1;
  for (const auto& t : carry_video_tiles_) busy[TileIndex(t)] = 1;
  bool any = false;
  size_t tiles = 0;
  for (size_t idx = 0; idx < f.n_tiles; ++idx) {
    if (!ledger_.HasPoor(TileRect(idx))) continue;
    any = true;
    const TileState& t = ledger_[idx];
    // (Unchanged for as long as the encoder rested: by time, as the frames
    // that would count it are not sent for it, see work_pending.)
    if (!now || busy[idx] || t.owed || t.stale != 0 || t.refine > 0 ||
        f.in.capture_time_us - std::min(f.in.capture_time_us, t.born_us) < kRetakeRestUs || InArea(idx) ||
        tiles >= kMaxRetakeTilesPerFrame) {
      continue;
    }
    tiles += PlanRetakesOfTile(f, idx);
  }
  if (!any) ledger_.ClearPoorPossible();
}

// The runs of poor cells of one tile to take again (at most
// kMaxRetakeRectsPerTile, the rest next time). False if none.
bool HostSession::PlanRetakesOfTile(Frame& f, size_t idx) {
  const Rect rect = TileRect(idx);
  std::vector<Rect> runs;
  for (int32_t y = rect.y; y < rect.bottom(); y += kCellSize) {
    const int32_t h = std::min(kCellSize, rect.bottom() - y);
    bool open = false;  // the last cell of this row went into runs.back()
    for (int32_t x = rect.x; x < rect.right(); x += kCellSize) {
      const Rect cell{x, y, std::min(kCellSize, rect.right() - x), h};
      const auto mix = ledger_.MixOf(cell);
      bool take = false;
      if (mix.poor && (mix.exact || ledger_.retakes(x, y) >= options_.text_video_retakes)) {
        // Exact pixels share the cell (a scroll by other than a multiple of
        // 16 brought them): taken from a picture they would blur, and coding
        // the tile again whole delayed the text a scroll had left owed. Left
        // as it is, as before 3.5. So is a cell retaken often enough.
        ledger_.Replace(cell, kPxPoor, kPxPolished);
      } else {
        take = mix.poor;
      }
      if (take && open) {
        runs.back().w += cell.w;
      } else if (take) {
        runs.push_back(cell);
      }
      open = take;
    }
  }
  if (runs.empty()) return false;
  if (runs.size() > kMaxRetakeRectsPerTile) runs.resize(kMaxRetakeRectsPerTile);
  const auto col = static_cast<int32_t>(idx % differ_.cols());
  const auto row = static_cast<int32_t>(idx / differ_.cols());
  for (const Rect& r : runs) {
    for (int32_t x = r.x; x < r.right(); x += kCellSize) ++ledger_.retakes(x, r.y);
    f.retakes.push_back({col, row, 0, 0, r});
  }
  f.trace.retake_rects += static_cast<int>(runs.size());
  return true;
}

bool HostSession::NothingToDo(const Frame& f) const {
  return f.changes.empty() && !f.key && ledger_.refining() == 0 && f.refresh.empty() && !f.regions_to_send &&
         !sharpen_video_ && !polish_owed_ && (!f.text || carry_video_tiles_.empty()) && f.retakes.empty();
}

// Tile cache, part 1: before this frame's updates, keep what the viewer
// shows for tiles that are about to change, if that content is likely to
// come back: it was on screen for a while (a window about to be covered),
// or it has been seen before (a cycling animation).
void HostSession::StoreLeavingTiles(Frame& f) {
  if (f.key) slots_.index.Clear();
  if (!f.key && options_.tile_cache) {
    // Recurrence alone is a weak signal (scrolling by a multiple of the tile
    // size makes every tile "recur"), so only a few such stores per frame.
    size_t recurring_budget = kMaxRecurringStores;
    for (const auto& c : f.changes) {
      if (c.prev_hash == 0) continue;
      TileState& t = ledger_[TileIndex(c)];
      const bool stable = frame_no_ - t.born >= kStableFrames;
      bool recurring = false;
      if (!stable && recurring_budget > 0) {
        auto seen = seen_.find(c.prev_hash);
        recurring = seen != seen_.end() && seen->second >= 2;
      }
      // (Not what the viewer does not have: see TileState::stale.)
      if ((stable || recurring) && !slots_.index.Find(c.prev_hash) && t.stale == 0) {
        if (!stable) --recurring_budget;
        const uint32_t slot = slots_.index.Insert(c.prev_hash);
        // prev_ still holds the previous frame here, i.e. the stored content.
        slots_.Keep(slot, c.rect, prev_.data(), width_ * 4, ledger_, t.quality);
        f.stores.push_back({slot, c.rect});
        t.slot = slot;
        t.slot_hash = c.prev_hash;
      }
    }
  }
  if (seen_.size() > kMaxSeen) seen_.clear();
}

// Which changed tiles are hot (changed in the frame before too), and
// whether the frame is motion.
void HostSession::ClassifyChanges(Frame& f) {
  // Time since the previous frame, at least one frame interval: longer when
  // the transport held frames back (or nothing changed for a while).
  const double frame_us = 1e6 / std::max(1, options_.fps);
  const double since_us =
      last_frame_us_ == 0 ? frame_us
                          : std::clamp(static_cast<double>(f.in.capture_time_us -
                                                           std::min(f.in.capture_time_us, last_frame_us_)),
                                       frame_us, 6 * frame_us);
  last_frame_us_ = f.in.capture_time_us;
  // Frames are being held back when several came late within a second (one
  // late frame is just the first change after a quiet spell).
  if (since_us > 1.5 * frame_us) {
    std::rotate(late_us_.begin(), late_us_.begin() + 1, late_us_.end());
    late_us_.back() = f.in.capture_time_us;
  }
  const bool holding_back = late_us_.front() != 0 && f.in.capture_time_us - late_us_.front() <= 1'000'000;
  holding_back_ = holding_back;
  f.hot.assign(f.changes.size(), false);
  for (size_t i = 0; i < f.changes.size(); ++i) {
    uint8_t& n = seen_[f.changes[i].hash];
    if (n < 255) ++n;
    TileState& t = ledger_[TileIndex(f.changes[i])];
    // Hot: changed in the previous frame too, or, while frames are being
    // held back, hot not long ago. A tile that is only partly video (the
    // edge of a video window) does not change in every frame, and coded
    // losslessly in between (up to 256 colours) it cost ~1 kB a time:
    // 10-20 kB frames, 0.3 s of a 0.4 Mbps link each, which held back the
    // frames after them, so more tiles missed a frame. (On a fast path the
    // hold would only cost text that scrolls: it went as video more often.)
    f.hot[i] = !f.key && (frame_no_ - t.born <= 1 ||
                          (holding_back && t.hot_us != 0 && f.in.capture_time_us - t.hot_us <= kHotHoldUs));
    t.hot_us = f.hot[i] ? f.in.capture_time_us : 0;
    t.born = frame_no_;
    t.born_us = f.in.capture_time_us;
  }
  size_t n_hot = 0;
  for (bool h : f.hot) n_hot += h;
  f.mass_motion = f.text && n_hot > kTextMaxHotTiles;
  f.trace.hot = static_cast<int>(n_hot);
}

// Scrolls and dragged windows: what moved becomes copies, and the pixels'
// qualities go along.
void HostSession::DetectMotion(Frame& f) {
  if (!f.key && options_.detect_moves) {
    // An unchanged tile is only made part of a move when the viewer shows
    // what would be copied into it exactly. Copied from tiles at video
    // quality (or stale ones), a lossless tile was left with a band or an
    // edge of slightly different colours, for good: it had not changed, so
    // nothing recorded that it was no longer exact.
    tile::MotionOptions motion;
    motion.may_copy_unchanged = [&](const Rect& src) {
      for (int32_t row = src.y / kTileSize; row <= (src.bottom() - 1) / kTileSize; ++row) {
        for (int32_t col = src.x / kTileSize; col <= (src.right() - 1) / kTileSize; ++col) {
          const TileState& t = ledger_[static_cast<size_t>(row) * differ_.cols() + col];
          if (t.quality != kExact || t.stale != 0) return false;
        }
      }
      return true;
    };
    f.moves = tile::DetectMoves({prev_.data(), width_, height_, width_ * 4}, f.in.bgra, f.changes, f.moved,
                                f.residual, motion);
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
    if (f.text) {
      for (size_t i = 0; i < f.changes.size(); ++i) {
        if (!f.moved[i] || !f.hot[i]) continue;
        int64_t left = 0;
        for (const Rect& r : f.residual[i]) left += static_cast<int64_t>(r.w) * r.h;
        if (left * 2 > static_cast<int64_t>(f.changes[i].rect.w) * f.changes[i].rect.h) {
          f.moved[i] = false;
          f.residual[i].clear();
        }
      }
    }
    size_t moved_count = 0;
    for (bool b : f.moved) moved_count += b;
    // (Tiles that are copied are not the ones in question.)
    size_t hot_unmoved = 0;
    for (size_t i = 0; i < f.changes.size(); ++i) hot_unmoved += f.hot[i] && !f.moved[i];
    if (hot_unmoved <= kTextMaxHotTiles) f.mass_motion = false;
    f.trace.hot_unmoved = static_cast<int>(hot_unmoved);
    f.trace.moved = static_cast<int>(f.moves.size());
    const double frame_us = 1e6 / std::max(1, options_.fps);
    const double hold = static_cast<double>(options_.bitrate_bps) / 8.0 * kLosslessBurstFrames * frame_us / 1e6;
    if (f.moves.size() > 64 && f.moves.size() > 6 * moved_count && options_.bitrate_bps > 0 &&
        kBytesPerMove * static_cast<double>(f.moves.size()) > 0.5 * hold) {
      f.moves.clear();
      f.moved.assign(f.moved.size(), false);
      for (auto& r : f.residual) r.clear();
    }
    // The encoder's picture is left as it was under moved tiles: the viewer
    // does not take video there, and unchanged pixels cost the encoder
    // nothing, while moved ones would make it code the motion.
  }
  // Text mode: a rect copied out of a tile the viewer does not have would
  // paint its placeholder (or its old content) over a tile that is fine.
  // Such copies are not sent; the tiles they would have filled are coded
  // whole instead (or wait, keeping what they show).
  if (f.text && !f.moves.empty()) {
    bool any = false;
    std::vector<uint8_t> from_stale(f.moves.size(), 0);
    for (size_t k = 0; k < f.moves.size(); ++k) {
      const Rect r = f.moves[k].src();
      for (int32_t row = r.y / kTileSize; row <= (r.bottom() - 1) / kTileSize && !from_stale[k]; ++row) {
        for (int32_t col = r.x / kTileSize; col <= (r.right() - 1) / kTileSize; ++col) {
          if (ledger_[static_cast<size_t>(row) * differ_.cols() + col].stale & kStalePlaceholder) {
            from_stale[k] = 1;
            break;
          }
        }
      }
      any |= from_stale[k] != 0;
    }
    if (any) {
      f.unmoved.assign(f.n_tiles, 0);
      std::vector<tile::Move> kept;
      for (size_t k = 0; k < f.moves.size(); ++k) {
        if (!from_stale[k]) {
          kept.push_back(f.moves[k]);
          continue;
        }
        const Rect& d = f.moves[k].dst;
        for (int32_t row = d.y / kTileSize; row <= (d.bottom() - 1) / kTileSize; ++row) {
          for (int32_t col = d.x / kTileSize; col <= (d.right() - 1) / kTileSize; ++col) {
            f.unmoved[static_cast<size_t>(row) * differ_.cols() + col] = 1;
          }
        }
      }
      f.moves = std::move(kept);
    }
  }
  // The pixels' quality goes where the pixels go (the moves are final here).
  ledger_.ApplyMoves(f.moves);
  // A video area's picture covers all of its tiles, changed or not.
  for (const auto& reg : regions_.regions()) ledger_.SetPixels(reg.rect, kPxVideo);
  for (const auto& m : f.moves) f.move_quality.push_back(QualityUnder(m.src()));
  // Whether a moved rect copies from a tile the viewer has older content
  // for: what it fills is then owed as well.
  f.move_tainted.assign(f.moves.size(), 0);
  if (f.text) {
    for (size_t k = 0; k < f.moves.size(); ++k) {
      const Rect r = f.moves[k].src();
      for (int32_t row = r.y / kTileSize; row <= (r.bottom() - 1) / kTileSize; ++row) {
        for (int32_t col = r.x / kTileSize; col <= (r.right() - 1) / kTileSize; ++col) {
          f.move_tainted[k] |= ledger_[static_cast<size_t>(row) * differ_.cols() + col].stale;
        }
      }
      f.any_tainted |= f.move_tainted[k] != 0;
    }
  }
}

// Codes every changed tile, and every tile whose owed turn it is.
void HostSession::CodeTiles(Frame& f) {
  f.refine_frames = static_cast<uint8_t>(std::clamp(f.refine_option, 0, 255));
  // What the lossless part of this frame may cost: a few frame intervals of
  // the bitrate (more after frames were held back). Tiles beyond the budget
  // go as video now; once static they are sent again losslessly, within the
  // budget of those frames.
  // A text mode keyframe may take half a second of the bitrate: the whole
  // screen's text then goes out with it on any path that is not slow, and
  // no tile is left a placeholder (while some are, everything a scroll or
  // a dragged window touches has to be coded whole, frame after frame).
  f.lossless_budget = f.text && f.key
                          ? std::max(bucket_.tokens(), static_cast<double>(options_.bitrate_bps) / 8.0 * 0.5)
                          : bucket_.tokens();
  f.prev_tile.resize(static_cast<size_t>(kTileSize) * kTileSize * 4);
  // Refreshed tiles are coded like changed ones (their content is current in
  // the frame, as they did not change).
  for (size_t idx : f.refresh) {
    f.changes.push_back({static_cast<int32_t>(idx % differ_.cols()), static_cast<int32_t>(idx / differ_.cols()), 0, 0,
                         TileRect(idx)});
  }
  f.n_changed = f.changes.size() - f.refresh.size();
  quiet_ = f.n_changed * 50 <= f.n_tiles;
  f.recoded.assign(f.text ? f.n_tiles : 0, 0);
  for (size_t i = 0; i < f.changes.size(); ++i) CodeChange(f, i);
  coarse_count_ = ledger_.coarse_count();
}

void HostSession::CodeChange(Frame& f, size_t i) {
  const tile::TileChange& c = f.changes[i];
  const size_t idx = TileIndex(c);
  TileState& t = ledger_[idx];
  // Owed tiles were picked by what a tile costs on average, and a tile of
  // dense text costs several times that: once the ones coded come to the
  // frame's share, the rest keep their turn for the next frame (the first
  // always goes).
  if (i >= f.n_changed && f.progressive_now[idx] && f.owed_spent > 0 && f.owed_spent >= f.owed_budget &&
      options_.bitrate_bps > 0) {
    f.progressive_now[idx] = 0;
    if (f.put_back == 0) ledger_.cursor = idx;
    ++f.put_back;
    --f.progressive_sent;
    return;
  }
  // The viewer does not have the tile as it was (text mode): nothing in
  // it can be taken as unchanged, so it is coded whole.
  const bool was_stale = t.stale != 0;
  if (f.text) f.recoded[idx] = 1;
  ledger_.NewContent(idx);
  if (i < f.n_changed) {
    t.coarse = false;  // new content
    for (int32_t y = 0; y < c.rect.h; ++y) {
      std::memcpy(f.prev_tile.data() + static_cast<size_t>(y) * kTileSize * 4,
                  prev_.data() + static_cast<size_t>(c.rect.y + y) * width_ * 4 + static_cast<size_t>(c.rect.x) * 4,
                  static_cast<size_t>(c.rect.w) * 4);
    }
    CopyRectInto(f.in.bgra, c.rect, prev_.data(), width_ * 4);
  }
  // Whatever happens below, the viewer must not take this tile from a
  // refining video picture any more (lossless tiles are flattened in it).
  ledger_.SetRefine(idx, 0);
  // Coded now (changed, or its owed turn): nothing owed any more.
  if (!f.key) ledger_.Settle(idx);
  if (i < f.n_changed && !f.moved.empty() && f.moved[i] && !(t.stale & kStalePlaceholder) &&
      !(!f.unmoved.empty() && f.unmoved[idx])) {
    CodeMovedTile(f, i, was_stale);
    return;
  }
  if (LoadFromCache(f, i)) return;
  if (CodeFromSlot(f, i, was_stale)) return;
  if (CodeSplitTile(f, i)) return;
  CodeWholeTile(f, i);
}

// A tile that moves arrives by copies; only the rows the moves leave (and
// what stayed put) are coded.
void HostSession::CodeMovedTile(Frame& f, size_t i, bool was_stale) {
  const tile::TileChange& c = f.changes[i];
  const size_t idx = TileIndex(c);
  TileState& t = ledger_[idx];
  ++f.moved_tiles;
  uint8_t q = f.MovedInQuality(c.rect);
  const bool tainted = was_stale || f.MovedInTainted(c.rect);
  if (tainted) {
    // The copies go (see kStalePlaceholder); the tile is owed whole.
    t.stale |= kStaleText;
    ledger_.Owe(idx);
    q = kRough;
  }
  if (!f.residual[i].empty()) q = std::min(q, t.quality);  // rows that stayed put
  // Only the rows that did not move or stay put are left to code, and of
  // those, what a dragged window just uncovered can come from the cache.
  // A tile that keeps changing while it moves (the edge of a dragged
  // window) can be left in dozens of crumbs, strips of 4 pixels and single
  // rows; each went out with a palette of its own, and the palettes came to
  // more than the pixels (1.7 MB of them in a 30 s drag in text mode). In
  // text mode more than kMaxResidualParts are coded as the one rect around
  // them: the moved pixels inside it are the screen's too, painted over the
  // copy with the same content.
  constexpr size_t kMaxResidualParts = 4;
  std::vector<Rect> to_code = f.residual[i];
  double min_share = 0.0;
  // (Text mode only: without it the merged rect goes to video more often,
  // and normal mode's picture is not made softer for traffic.)
  if (f.text && f.hot[i] && to_code.size() > kMaxResidualParts) {
    int32_t x0 = to_code[0].x, y0 = to_code[0].y, x1 = to_code[0].right(), y1 = to_code[0].bottom();
    for (const Rect& r : to_code) {
      x0 = std::min(x0, r.x);
      y0 = std::min(y0, r.y);
      x1 = std::max(x1, r.right());
      y1 = std::max(y1, r.bottom());
    }
    to_code = {Rect{x0, y0, x1 - x0, y1 - y0}};
    min_share = 0.5;  // the cache only if it covers most of it (else crumbs again)
  }
  for (const Rect& r : to_code) {
    for (const Rect& part : PaintFromSlot(f, c, r, min_share, q)) {
      if (CodeRect(f, c, part, f.hot[i])) q = kRough;
    }
  }
  // Text mode: what was copied from video quality may be text that went as
  // video while it moved; it is owed exactly like the tile it came from
  // (which is not where it was any more). Whether it was, is looked up
  // pixel by pixel: a move is as good as the worst tile under its source,
  // and a scroll is one move of the whole page, so by tiles the page was
  // owed again after every scroll for one icon in it.
  bool owed = f.text && q != kExact;
  if (owed && !tainted) {
    const uint8_t worst = ledger_.WorstPixel(c.rect);
    if (worst == kPxExact) {
      q = kExact;
      owed = false;
    } else if (worst == kPxPolished || worst == kPxPoor) {
      // Text where it is text, and video that is finished: coding it
      // again would send the same picture. (Poor video is taken again
      // where it is now, see PlanRetakes.)
      q = std::max(q, kRefined);
      owed = false;
    }
  }
  t.quality = q;
  if (owed) ledger_.Owe(idx);
}

// Tile cache, part 2: content the viewer has kept. Like moves, the
// encoder's picture is left alone here.
bool HostSession::LoadFromCache(Frame& f, size_t i) {
  if (i >= f.n_changed || f.key || !options_.tile_cache) return false;
  const tile::TileChange& c = f.changes[i];
  const auto slot = slots_.index.Find(c.hash);
  if (!slot) return false;
  const size_t idx = TileIndex(c);
  TileState& t = ledger_[idx];
  f.loads.push_back({*slot, c.rect});
  slots_.Restore(*slot, c.rect, c.rect, ledger_);
  t.quality = slots_.quality(*slot);
  t.stale = 0;
  // Owed only where some of it is video that is not finished (text that
  // went as video): a tile that was split comes back with its text exact
  // and the rest finished, and coding it again would send the same.
  if (f.text && t.quality != kExact && ledger_.WorstPixel(c.rect) == kPxVideo) ledger_.Owe(idx);
  return true;
}

// A tile that was static and now changes partly (typing into a line that
// was stored, a menu closing) may get most of its pixels from the slot; hot
// tiles (video, animation) are left whole.
bool HostSession::CodeFromSlot(Frame& f, size_t i, bool was_stale) {
  if (i >= f.n_changed || f.hot[i] || was_stale) return false;
  const tile::TileChange& c = f.changes[i];
  const size_t idx = TileIndex(c);
  TileState& t = ledger_[idx];
  // What stays of a rough tile stays rough (it is sent again once the
  // screen is still); calling it exact left it as it was for good.
  uint8_t q = t.quality == kRough ? kRough : kExact;
  const auto parts = PaintFromSlot(f, c, c.rect, 0.25, q);
  if (parts.size() == 1 && parts[0] == c.rect) return false;
  for (const Rect& part : parts) {
    if (CodeRect(f, c, part, f.hot[i])) q = kRough;
  }
  t.quality = q;
  if (f.text && q != kExact) ledger_.Owe(idx);
  return true;
}

// Text mode (HostOptions::split_cells): a tile that does not fit a palette
// as a whole is looked at in 16 x 16 cells. Its cells of text
// (tile::LooksLikeText: text beside a shadow, an icon or a translucent
// panel) go out losslessly, and so do other cells of few colours; the rest
// goes through the encoder, once: such a tile is not refined or polished
// (the picture has its lossless cells flattened, and taking the whole tile
// from it again would paint over them).
bool HostSession::CodeSplitTile(Frame& f, size_t i) {
  if (options_.split_cells <= 0 || !f.text || (i < f.n_changed && f.hot[i] && (f.mass_motion || !options_.split_hot))) {
    return false;
  }
  const tile::TileChange& c = f.changes[i];
  codec::PaletteTile probe;
  if (codec::ExtractReducedPalette(f.in.bgra, c.rect, options_.text_colors, options_.lossless_max_colors, 4096, probe,
                                   nullptr, /*faithful=*/true)) {
    return false;
  }
  constexpr int32_t kCell = 16;
  std::vector<Rect> exact_runs, video_runs, as_is;
  for (int32_t y = c.rect.y; y < c.rect.bottom(); y += kCell) {
    const int32_t h = std::min(kCell, c.rect.bottom() - y);
    int last = -1;  // 1 = the open run is lossless, 0 = video
    for (int32_t x = c.rect.x; x < c.rect.right(); x += kCell) {
      const Rect cell{x, y, std::min(kCell, c.rect.right() - x), h};
      // Text always goes out losslessly: in reduced colours where that
      // stays faithful, else as it is (a cell has at most 256 colours).
      // Anything else only where it is cheap.
      const bool reduces = codec::ExtractReducedPalette(f.in.bgra, cell, options_.text_colors,
                                                        options_.lossless_max_colors, 4096, probe, nullptr, true);
      const bool is_text = tile::LooksLikeText(f.in.bgra, cell);
      if (is_text && !reduces) {
        as_is.push_back(cell);
        last = -1;
        continue;
      }
      const bool fits = reduces && (is_text || static_cast<int>(probe.colors.size()) <= options_.split_cells);
      auto& runs = fits ? exact_runs : video_runs;
      if (last == (fits ? 1 : 0)) {
        runs.back().w += cell.w;
      } else {
        runs.push_back(cell);
      }
      last = fits ? 1 : 0;
    }
  }
  if (exact_runs.empty() && as_is.empty()) return false;
  const size_t idx = TileIndex(c);
  TileState& t = ledger_[idx];
  bool video = !video_runs.empty(), deferred = false;
  // One rect around the cells for the encoder (each part costs the region
  // list its bytes, and more than a few make the tile rough again); the
  // lossless runs inside it are flattened below and painted over the
  // picture by the viewer.
  if (video) {
    Rect b = video_runs.front();
    for (const Rect& r : video_runs) {
      const int32_t x0 = std::min(b.x, r.x), y0 = std::min(b.y, r.y);
      const int32_t x1 = std::max(b.right(), r.right()), y1 = std::max(b.bottom(), r.bottom());
      b = {x0, y0, x1 - x0, y1 - y0};
    }
    picture_.Copy(f.in.bgra, b);
    tile::TileChange part = c;
    part.rect = b;
    f.video_tiles.push_back(part);
    // What is not text gets no more than this picture: finished (the
    // lossless runs below are set exact again).
    ledger_.SetPixels(b, kPxPolished);
  }
  // A run goes out as one rect where its cells share a palette; where they
  // do not (each fits one, the run does not), cell by cell. What then still
  // goes to the encoder is over the lossless budget: text as video for
  // now, owed, and the tile not finished.
  for (const Rect& r : exact_runs) {
    if (r.w <= kCell || codec::ExtractReducedPalette(f.in.bgra, r, options_.text_colors, options_.lossless_max_colors,
                                                     4096, probe, nullptr, true)) {
      deferred |= CodeRect(f, c, r, false) != kCodedLossless;
      continue;
    }
    for (int32_t x = r.x; x < r.right(); x += kCell) {
      deferred |= CodeRect(f, c, Rect{x, r.y, std::min(kCell, r.right() - x), r.h}, false) != kCodedLossless;
    }
  }
  for (const Rect& r : as_is) {
    codec::PaletteTile p;
    if (!codec::ExtractPalette(f.in.bgra, r, 256, p)) continue;  // (cannot happen: 256 pixels)
    picture_.Fill(r, p.colors[0]);
    const size_t n = p.colors.size();
    f.lossless_spent += (3.0 * static_cast<double>(n) + static_cast<double>(r.w) * r.h) * (f.text ? lossless_ratio_ : 1.0);
    f.lossless_raw += 3.0 * static_cast<double>(n) + static_cast<double>(r.w) * r.h;
    f.lossless_tiles.push_back(std::move(p));
    f.lossless_px.push_back(kPxExact);
    ++f.trace.lossless_rects;
  }
  if (deferred) {
    t.quality = kRough;
  } else {
    t.quality = video ? kRefined : kExact;
    t.stale = 0;
  }
  t.coarse = false;
  return true;
}

void HostSession::CodeWholeTile(Frame& f, size_t i) {
  const tile::TileChange& c = f.changes[i];
  TileState& t = ledger_[TileIndex(c)];
  const int how = CodeRect(f, c, c.rect, i < f.n_changed && f.hot[i]);
  if (how == kDeferred) {
    t.quality = kRough;
  } else if (how == kCodedVideo) {
    f.refine_after.push_back(i);
    t.quality = kRough;
    // (A text mode keyframe's placeholder is in the picture, not the tile.)
    t.stale = f.text && f.key && t.owed ? kStaleText : 0;
  } else {
    // (A quick pass leaves it rough: it is sent again once the screen is
    // still, like a tile left at video quality.)
    t.quality = f.last_coarse ? kRough : kExact;
    t.coarse = f.last_coarse;
    t.stale = 0;
  }
}

// Codes `r` (all or part of tile `c`) losslessly if it has few colours,
// else through the video encoder. Returns kCodedLossless, kCodedVideo, or
// (text mode) kDeferred: nothing was sent, the tile is stale and owed.
int HostSession::CodeRectAs(Frame& f, const tile::TileChange& c, const Rect& r, bool is_hot) {
  const size_t idx = TileIndex(c);
  // Lossless pays off on content that stays, or on a thin sliver; content
  // that keeps changing is cheaper as video unless it is very simple.
  const bool sliver = r.h * 4 <= kTileSize || r.w * 4 <= kTileSize;
  int max_colors = is_hot && !sliver ? options_.lossless_max_colors_hot : options_.lossless_max_colors;
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
  f.last_coarse = false;
  const bool in_motion = f.text && f.mass_motion && is_hot && !sliver && !f.key && !f.progressive_now[idx];
  if (in_motion) {
    palette = codec::ExtractPalette(f.in.bgra, r, max_colors, t);
    if (!palette) {
      ledger_.Owe(idx);
      ++f.trace.video_motion;
    }
  } else if (f.text) {
    // The quick pass: a whole tile on its turn, which the viewer has
    // nothing useful for (stale, or a keyframe's), once.
    if (f.coarse_now && f.progressive_now[idx] && !ledger_[idx].coarse &&
        (progressive_active_ || (ledger_[idx].stale & kStaleText))) {
      palette = codec::ExtractReducedPalette(f.in.bgra, r, kTextCoarseColors, options_.lossless_max_colors, 4096, t,
                                             &f.last_coarse);
    }
    if (!palette) {
      f.last_coarse = false;
      // Faithful: text in several colours gets 32 or 64 instead of going
      // to the video encoder (two thirds of a page of highlighted code
      // on a dark background did, at 16).
      palette = codec::ExtractReducedPalette(f.in.bgra, r, options_.text_colors, options_.lossless_max_colors, 4096, t,
                                             nullptr, /*faithful=*/true);
      // Not text (a gradient, a wallpaper, an icon): the video encoder's,
      // which is good at these. (Exact, as without text mode for up to 256
      // colours, such a tile costs 2-3 kB; a scroll over a textured
      // background came to more traffic than without text mode.)
    }
  } else {
    palette = options_.lossless && codec::ExtractPalette(f.in.bgra, r, max_colors, t);
  }
  auto to_video = [&] {
    picture_.Copy(f.in.bgra, r);
    tile::TileChange part = c;
    part.rect = r;
    f.video_tiles.push_back(part);
    return kCodedVideo;
  };
  if (palette) {
    // (Text mode has no video-first keyframe: its tiles go out with the
    // keyframe as far as the budget reaches, see below.)
    if (f.progressive_key && !f.text && t.colors.size() > 1) {
      // Video now, lossless later (only whole tiles are keyframe changes).
      // Plain tiles cost next to nothing either way and go right away.
      ledger_.Owe(idx);
      ++f.trace.video_keyframe_first;
      return to_video();
    }
    const bool progressive_turn = f.progressive_now[idx] != 0;
    const size_t n = t.colors.size();
    const int bits = n <= 1 ? 0 : n <= 2 ? 1 : n <= 4 ? 2 : n <= 16 ? 4 : 8;
    // Plus the rect and the palette size (a plain sliver costs that alone).
    const double raw = 3.0 * static_cast<double>(n) + static_cast<double>(r.w) * r.h * bits / 8.0;
    // Text mode counts a tile at what tiles have been deflating to, and
    // lets the first of a frame through while the bucket is not overdrawn
    // (at these rates one tile is more than a frame's share).
    const double cost = f.text ? raw * lossless_ratio_ : raw * kLosslessRawShare;
    const bool over = f.text ? f.lossless_budget <= 0 || (f.lossless_spent > 0 && f.lossless_spent + cost > f.lossless_budget)
                             : f.lossless_spent + cost > f.lossless_budget;
    if ((!f.key || f.text) && !progressive_turn && n > 1 && options_.bitrate_bps > 0 && over) {
      f.over_lossless_budget = true;
      // Owed: sent losslessly once it stops changing, within the bucket
      // (the same queue as a progressive keyframe's tiles). Without this
      // the tile stayed at video quality (refresh is off by default).
      ledger_.Owe(idx);
      // Text mode waits only where it must: with a keyframe, whose picture
      // covers the whole screen and shows the tile's background colour
      // until its text comes (on a path that takes the screen at once,
      // none is left like that). The viewer does not have such a tile: a
      // caret blinking in it before its turn stored the placeholder in the
      // tile cache and painted it back at every blink, and a partial change
      // left the rest of it a block of plain colour for good. Otherwise as
      // without text mode, video now and exact later: a tile left as it was
      // among tiles that are new looks worse than one that is soft for a
      // moment.
      if (f.text && f.key) {
        ledger_[idx].stale |= kStaleText | kStalePlaceholder;
        picture_.Fill(r, t.colors[0]);
        return kDeferred;
      }
      ++f.trace.video_budget;
      return to_video();
    }
    f.lossless_spent += cost;
    f.lossless_raw += raw;
    if (progressive_turn) {
      f.owed_spent += raw * lossless_ratio_;
      // Flattening it now would make the encoder code the change, and at a
      // low bitrate that costs the tiles still refining around it.
      ledger_[idx].unflattened = true;
    } else {
      picture_.Fill(r, t.colors[0]);
    }
    f.lossless_tiles.push_back(std::move(t));
    ++f.trace.lossless_rects;
    return kCodedLossless;
  }
  if (f.tracing && !in_motion) {
    // Too many colours for any palette, or only for a hot rect's?
    codec::PaletteTile whole;
    if (!f.text && max_colors < options_.lossless_max_colors &&
        codec::ExtractPalette(f.in.bgra, r, options_.lossless_max_colors, whole)) {
      ++f.trace.video_hot;
    } else {
      ++f.trace.video_colours;
    }
  }
  return to_video();
}

int HostSession::CodeRect(Frame& f, const tile::TileChange& c, const Rect& r, bool is_hot) {
  const int how = CodeRectAs(f, c, r, is_hot);
  // (The quick pass's colours are not the tile's own: it is sent again.)
  const uint8_t px = how == kCodedLossless && !f.last_coarse ? kPxExact : kPxVideo;
  ledger_.SetPixels(r, px);
  if (how == kCodedLossless) f.lossless_px.push_back(px);
  return how;
}

// Paints the parts of `r` (inside tile `c`) that equal what the tile's last
// stored slot holds from that slot, in 16-pixel strips. Returns the parts
// still to code; lowers `q` to the slot's quality if the slot was used.
// Gives up (returns `r` whole) when less than `min_share` of it matches.
std::vector<Rect> HostSession::PaintFromSlot(Frame& f, const tile::TileChange& c, const Rect& r, double min_share,
                                             uint8_t& q) {
  const TileState& t = ledger_[TileIndex(c)];
  if (f.key || !options_.tile_cache || t.slot < 0 || !slots_.index.Holds(static_cast<uint32_t>(t.slot), t.slot_hash)) {
    return std::vector<Rect>{r};
  }
  const auto slot = static_cast<uint32_t>(t.slot);
  constexpr int32_t kStrip = 16;
  const uint8_t* base = slots_.pixels(slot);
  const size_t first_load = f.loads.size();
  std::vector<Rect> rest;
  int64_t matched = 0;
  for (int32_t x0 = r.x; x0 < r.right(); x0 += kStrip) {
    const int32_t sw = std::min(kStrip, r.right() - x0);
    // 'U' unchanged since the previous frame (the viewer already shows it),
    // 'S' from the slot, 'X' to code.
    auto kind = [&](int32_t y) {
      const uint8_t* cur = f.in.bgra.data + static_cast<size_t>(y) * f.in.bgra.stride + x0 * 4;
      const size_t off = (static_cast<size_t>(y - c.rect.y) * kTileSize + (x0 - c.rect.x)) * 4;
      const size_t n = static_cast<size_t>(sw) * 4;
      if (std::memcmp(cur, f.prev_tile.data() + off, n) == 0) return 'U';
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
        f.loads.push_back({slot, run, static_cast<uint32_t>(x0 - c.rect.x), static_cast<uint32_t>(y - c.rect.y)});
        matched += static_cast<int64_t>(run.w) * run.h;
        // (If the slot is given up below, the caller codes all of `r`.)
        slots_.Restore(slot, c.rect, run, ledger_);
      } else {
        rest.push_back(run);
      }
      y = e;
    }
  }
  if (matched == 0 || matched < min_share * r.w * r.h) {
    f.loads.resize(first_load);
    return std::vector<Rect>{r};
  }
  q = std::min(q, slots_.quality(slot));
  // Join strips that cover the same rows.
  std::sort(rest.begin(), rest.end(), [](const Rect& a, const Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
  std::vector<Rect> merged;
  for (const Rect& p : rest) {
    if (!merged.empty() && merged.back().y == p.y && merged.back().h == p.h && merged.back().right() == p.x) {
      merged.back().w += p.w;
    } else {
      merged.push_back(p);
    }
  }
  return merged;
}

// What the viewer takes from this frame's picture, and the picture itself.
void HostSession::AssemblePicture(Frame& f) {
  ListCarriedTiles(f);
  for (const auto& r : f.retakes) {
    picture_.Copy(f.in.bgra, r.rect);  // (moved there, or the screen's already)
    f.region_tiles.push_back(r);
  }
  CoarsenVideoParts(f);
  ContinueRefinement(f);
  // A progressive keyframe's picture is left as it is until the text is
  // done: refining it at the bitrate left over made it worse, not better,
  // and the intra picture at the end replaces it anyway.
  if (f.progressive_key) f.refine_after.clear();
  FinishVideoTiles(f);
  SharpenAfterProgressive(f);
  PolishStillScreen(f);
  // What the viewer takes from the picture is video from now on, except
  // where a lossless rect of this frame is painted over it.
  for (const auto& t : f.region_tiles) ledger_.TakenFromVideo(t.rect);
  for (size_t i = 0; i < f.lossless_tiles.size(); ++i) ledger_.SetPixels(f.lossless_tiles[i].rect, f.lossless_px[i]);
  if (!f.key) picture_.SurroundTaken(f.in.bgra, f.region_tiles, regions_, differ_.cols(), differ_.rows());
  if (f.text) picture_.KeepScreenNextToVideo(f.in.bgra, f.region_tiles, f.lossless_tiles);
}

// The viewer takes this frame's video tiles, and those of a picture the
// encoder dropped.
void HostSession::ListCarriedTiles(Frame& f) {
  // Text mode: a tile coded again is no longer taken from a picture that
  // was held back (if it is video again, it is listed anew). Listing it
  // with a later picture would paint that picture's flattened tile over
  // text the viewer has received since.
  if (f.text && !carry_video_tiles_.empty()) {
    std::erase_if(carry_video_tiles_, [&](const tile::TileChange& t) { return f.recoded[TileIndex(t)] != 0; });
  }
  f.region_tiles = f.video_tiles;
  if (carry_video_tiles_.empty()) return;
  // Every part of a dropped picture is listed again (a tile may have had
  // several, and this frame may code another part of it), once, unless
  // this frame lists its tile whole anyway.
  std::vector<uint8_t> whole(f.n_tiles, 0);
  std::set<std::tuple<int32_t, int32_t, int32_t, int32_t>> listed;
  for (const auto& t : f.region_tiles) {
    listed.insert({t.rect.x, t.rect.y, t.rect.w, t.rect.h});
    if (t.rect.w == kTileSize && t.rect.h == kTileSize) whole[TileIndex(t)] = 1;
  }
  for (const auto& t : carry_video_tiles_) {
    if (whole[TileIndex(t)]) continue;
    if (listed.insert({t.rect.x, t.rect.y, t.rect.w, t.rect.h}).second) f.region_tiles.push_back(t);
  }
  carry_video_tiles_.clear();
}

// A tile left in many video parts (the strips and rows a scroll leaves
// between moved ones) is listed as one rect around them: each part costs
// the region list ~6 bytes, and on a busy scroll at 0.4 Mbps frames listed
// over a thousand of them (6.7 kB, four times the picture). The pixels of
// the rect the encoder did not get yet (moved ones; lossless ones are
// painted over the picture anyway) are given to it now, and the tile is
// left rough, to be refreshed once static.
void HostSession::CoarsenVideoParts(Frame& f) {
  constexpr size_t kMaxPartsPerTile = 4;
  std::unordered_map<size_t, std::pair<size_t, Rect>> per_tile;  // parts, bounds
  for (const auto& t : f.region_tiles) {
    auto [it, fresh] = per_tile.try_emplace(TileIndex(t), 0, t.rect);
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
  if (!coarsened) return;
  std::vector<tile::TileChange> kept;
  kept.reserve(f.region_tiles.size());
  std::vector<uint8_t> done(f.n_tiles, 0);
  for (const auto& t : f.region_tiles) {
    const size_t idx = TileIndex(t);
    const auto& pb = per_tile[idx];
    if (pb.first <= kMaxPartsPerTile) {
      kept.push_back(t);
      continue;
    }
    if (done[idx]) continue;
    done[idx] = 1;
    picture_.Copy(f.in.bgra, pb.second);
    // Lossless parts stay flattened, as everywhere else.
    for (const auto& lt : f.lossless_tiles) {
      if (lt.rect.intersects(pb.second)) picture_.Fill(lt.rect, lt.colors[0]);
    }
    tile::TileChange whole = t;
    whole.rect = pb.second;
    kept.push_back(whole);
    ledger_[idx].quality = kRough;
  }
  f.region_tiles = std::move(kept);
}

// Tiles still refining are taken from this picture too.
void HostSession::ContinueRefinement(Frame& f) {
  if (ledger_.refining() == 0) return;
  for (int32_t row = 0; row < differ_.rows(); ++row) {
    for (int32_t col = 0; col < differ_.cols(); ++col) {
      const size_t idx = static_cast<size_t>(row) * differ_.cols() + col;
      if (ledger_[idx].refine == 0) continue;
      f.region_tiles.push_back({col, row, 0, 0, TileRect(idx)});
      ++f.trace.refine_tiles;
      // The encoder has had its chance to sharpen this tile.
      if (ledger_.Refined(idx)) ledger_[idx].quality = kRefined;
    }
  }
}

// Tiles that went to video whole start their refinement; with none planned,
// what they got is all the video they get.
void HostSession::FinishVideoTiles(Frame& f) {
  for (size_t i : f.refine_after) {
    const size_t idx = TileIndex(f.changes[i]);
    ledger_.SetRefine(idx, f.refine_frames);
    if (f.refine_frames != 0) continue;
    TileState& t = ledger_[idx];
    if (f.text && ((i < f.n_changed && f.hot[i]) || t.owed)) {
      // Text mode without refinement: a tile that keeps changing was not
      // looked at by cells. It stays rough, so it is owed once it stops and
      // its text is split out then (typing beside an icon or a shadow). So
      // does one that is owed already: it may be text that went as video
      // (in motion, over the budget), and must not count as finished when
      // it comes back from the tile cache before its turn.
      continue;
    }
    // With no refinement planned, a refresh would not do better either.
    t.quality = kRefined;
    // ... and in text mode this is all the video it gets: finished, so that
    // a scroll does not owe it again (see kPxPolished). Not where it is
    // owed: that may be text that went as video.
    if (f.text) ledger_.SetPixels(f.changes[i].rect, kPxPolished);
  }
}

// The end of a progressive keyframe: the text has gone out losslessly, so
// the encoder's picture is now mostly flat and a fresh intra picture of it
// spends its bits on what is left (photos, gradients), which the first
// picture had to share with all the text. Only tiles where the encoder's
// input still matches the screen (not moved ones) are taken from it.
void HostSession::SharpenAfterProgressive(Frame& f) {
  f.sharpen = sharpen_video_ && !f.key;
  sharpen_video_ = false;
  if (!f.sharpen) return;
  // Flatten the text the progressive tiles left in the encoder's input
  // (where it is still there), so the intra picture does not spend on it.
  for (size_t idx = 0; idx < f.n_tiles; ++idx) {
    TileState& t = ledger_[idx];
    if (!t.unflattened) continue;
    t.unflattened = false;
    if (t.quality != kExact) continue;
    const Rect rect = TileRect(idx);
    if (!picture_.Matches(prev_, rect)) continue;  // recoded since
    picture_.Fill(rect, picture_.ColourAt(rect.x, rect.y));
  }
  std::vector<uint8_t> listed(f.n_tiles, 0);
  for (const auto& t : f.region_tiles) listed[TileIndex(t)] = 1;
  for (int32_t row = 0; row < differ_.rows(); ++row) {
    for (int32_t col = 0; col < differ_.cols(); ++col) {
      const size_t idx = static_cast<size_t>(row) * differ_.cols() + col;
      if (listed[idx] || ledger_[idx].quality == kExact || regions_.RegionOf(col, row) >= 0) continue;
      const Rect rect = TileRect(idx);
      if (!picture_.Matches(prev_, rect)) continue;
      f.region_tiles.push_back({col, row, 0, 0, rect});
      ledger_.SetRefine(idx, f.refine_frames);
      ledger_[idx].quality = f.refine_frames == 0 ? kRefined : kRough;
    }
  }
}

// Still-screen polish: with the screen nearly still, the tiles left at
// video quality are taken from the next pictures too, and those are coded
// with the lower QP floor (HostOptions::still_refine_frames).
void HostSession::PolishStillScreen(Frame& f) {
  f.still = f.still_frames > 0 && quiet_ && !f.key && !f.sharpen;
  if (!f.still || !polish_owed_) return;
  std::vector<uint8_t> listed(f.n_tiles, 0);
  for (const auto& t : f.region_tiles) listed[TileIndex(t)] = 1;
  for (int32_t row = 0; row < differ_.rows(); ++row) {
    for (int32_t col = 0; col < differ_.cols(); ++col) {
      const size_t idx = static_cast<size_t>(row) * differ_.cols() + col;
      TileState& t = ledger_[idx];
      if (t.polish == 0) continue;
      if (t.quality == kExact) {
        t.polish = 0;
        continue;
      }
      // Not yet: still in its ordinary refinement, about to be sent
      // losslessly, or part of a video area.
      if (listed[idx] || t.refine > 0 || t.owed || regions_.RegionOf(col, row) >= 0) continue;
      const Rect rect = TileRect(idx);
      if (t.polish == kUnpolished) {
        if (frame_no_ - t.born < kRefreshDelay) continue;  // may still be changing
        // Only what the encoder's picture really shows: a tile the viewer
        // got by a move or from the cache is not in it.
        if (!picture_.Matches(prev_, rect)) {
          t.polish = 0;
          continue;
        }
        t.polish = f.still_frames;
      }
      f.region_tiles.push_back({col, row, 0, 0, rect});
      ++f.trace.polish_tiles;
      if (--t.polish == 0) {
        t.quality = kRefined;
        ledger_.SetPixels(rect, kPxPolished);
      }
    }
  }
}

// Codes the picture (if the viewer takes anything from it). False with
// `done` set: the encoder failed; `done` alone: it dropped a keyframe, and
// the frame goes no further.
bool HostSession::EncodePicture(Frame& f, std::string* error, bool* done) {
  if (!f.key && f.region_tiles.empty()) return true;
  encoder_->SetMinQp(f.still ? options_.still_min_qp : 0);
  // (Text mode ends a progressive keyframe without the intra picture: the
  // tiles left at the first picture's quality are refined by the next.)
  f.au = encoder_->Encode(picture_.view(), f.in.capture_time_us, f.key || (f.sharpen && !f.text));
  f.picture_coded = f.au.has_value();
  if (f.au) last_picture_us_ = f.in.capture_time_us;
  if (f.au && f.text && mirror_) f.mirrored = f.au;
  if (f.au && f.text) {
    for (size_t idx = 0; idx < f.n_tiles; ++idx) ledger_[idx].stale &= static_cast<uint8_t>(~kStaleVideo);
  }
  if (!f.au && !f.key && encoder_->last_error().empty()) {
    // The viewer does not get these tiles' new content with this frame.
    if (f.text) {
      for (const auto& t : f.video_tiles) ledger_[TileIndex(t)].stale |= kStaleVideo;
    }
    // The encoder chose to drop this picture (VideoToolbox does at low
    // rates). Its references are intact and its input already holds this
    // frame, so the next picture carries these changes: send the rest of
    // the frame as usual and list its video tiles with the next picture.
    // (A full refresh here made the next frame a keyframe, which made the
    // encoder drop more: at 1 Mbps most frames were lost that way.)
    carry_video_tiles_.insert(carry_video_tiles_.end(), f.region_tiles.begin(), f.region_tiles.end());
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.encoder_drops;
    return true;
  }
  if (f.au) return true;
  // The encoder failed (or dropped a keyframe); make sure the next one
  // fully refreshes the viewer.
  force_keyframe_ = true;
  differ_.Reset();
  ledger_.ForgetAll();
  *done = true;
  // An empty error means the encoder chose to drop the frame, which is
  // not a failure.
  const std::string& why = encoder_->last_error();
  if (why.empty()) return true;
  if (error) *error = why;
  std::lock_guard<std::mutex> lock(mu_);
  ++stats_.encode_errors;
  return false;
}

// The video areas' pictures, the lossless part, and the frame on its way.
void HostSession::SendFrame(Frame& f) {
  const uint64_t id = next_frame_id_++;
  std::vector<uint8_t> region_bytes;
  size_t region_payload = 0, region_pictures = 0;
  uint64_t region_us = 0;
  if (f.n_regions > 0 &&
      !EncodeRegions(f.in, f.region_dirty, id, region_bytes, region_payload, region_pictures, region_us)) {
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
  if (f.use_regions) {
    for (const auto& t : f.video_tiles) f.busy[TileIndex(t)] = 1;
    FinishRegions(f.busy, f.region_changed, f.in.capture_time_us);
  }

  // Deflate only after the video encode succeeded: the stream's history must
  // match what the viewer actually receives.
  const size_t n_lossless = f.lossless_tiles.size();
  const size_t n_loads = f.loads.size();
  codec::RegionOps ops;
  ops.stores = std::move(f.stores);
  for (const auto& m : f.moves) ops.moves.push_back({m.dst, m.dx, m.dy});
  ops.loads = std::move(f.loads);
  if (f.text && options_.context_tiles) {
    for (auto& t : f.lossless_tiles) {
      const size_t n = t.colors.size();
      const size_t most = options_.context_tiles_64 ? 64 : 16;
      (n >= 2 && n <= most ? ops.context_tiles : ops.tiles).push_back(std::move(t));
    }
  } else {
    ops.tiles = std::move(f.lossless_tiles);
  }
  std::vector<uint8_t> lossless_data, context_data;
  if (!ops.empty()) lossless_data = lossless_.Encode(ops, f.key, &context_data);
  const size_t lossless_size = lossless_data.size() + context_data.size();
  if (progressive_active_ && !f.key && ledger_.owed_count() == 0) {
    progressive_active_ = false;
    // Next frame, if any tile is left at the first picture's quality.
    sharpen_video_ = false;
    for (size_t idx = 0; idx < f.n_tiles && !sharpen_video_; ++idx) sharpen_video_ = ledger_[idx].quality != kExact;
  }
  if (f.progressive_sent > 0 && n_lossless > 0) {
    lossless_bytes_per_tile_ = 0.7 * lossless_bytes_per_tile_ +
                               0.3 * static_cast<double>(lossless_size) / static_cast<double>(n_lossless);
  }
  if (n_lossless > 0 && f.lossless_raw > 256) {
    lossless_ratio_ = std::clamp(0.8 * lossless_ratio_ + 0.2 * static_cast<double>(lossless_size) / f.lossless_raw, 0.05, 1.0);
  }
  const uint64_t t3 = capture::MonotonicMicros();
  if (f.text && !f.au && region_bytes.empty() && lossless_data.empty()) {
    // Everything this frame changed waits (for the lossless budget, or for
    // the next video picture): no frame goes out. At these rates even an
    // empty one a frame interval adds up.
    --next_frame_id_;
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.frames_skipped;
    return;
  }

  std::vector<uint8_t> bytes;
  proto::EncodeTo(proto::FrameBegin{id, f.in.capture_time_us}, bytes);
  size_t video_bytes = 0;
  if (f.au) {
    proto::VideoData vd;
    vd.frame_id = id;
    vd.keyframe = f.au->keyframe;
    vd.parameter_sets = std::move(f.au->parameter_sets);
    vd.nal_data = std::move(f.au->nal_data);
    // Only a requested keyframe covers the whole screen: the viewer must not
    // take flattened lossless tiles from any other picture.
    if (!f.key) vd.regions = TilesToRegions(f.region_tiles);
    video_bytes = vd.nal_data.size();
    proto::EncodeTo(vd, bytes);
  }
  // Area pictures go on top of the full-screen one.
  bytes.insert(bytes.end(), region_bytes.begin(), region_bytes.end());
  if (!lossless_data.empty()) {
    proto::EncodeTo(proto::LosslessData{id, f.key, lossless_data, context_data}, bytes);
  }
  proto::EncodeTo(proto::FrameEnd{id}, bytes);
  transport_->Send(transport::Channel::kVideo, bytes);

  std::lock_guard<std::mutex> lock(mu_);
  ++stats_.frames_encoded;
  if (f.au) {
    ++stats_.video_frames;
    if (f.au->keyframe) ++stats_.keyframes;
    stats_.encode_us_total += f.t2 - f.t1;
  }
  stats_.bytes_sent += bytes.size();
  stats_.video_bytes += video_bytes + region_payload;
  stats_.region_bytes += region_payload;
  stats_.region_pictures += region_pictures;
  stats_.region_encode_us_total += region_us;
  stats_.lossless_bytes += lossless_size;
  bucket_.Charge(static_cast<double>(lossless_size));
  // (That keyframe is not held against the frames after it.)
  if (f.text && f.key) bucket_.Forgive();
  stats_.lossless_tiles += n_lossless;
  stats_.moved_tiles += f.moved_tiles;
  moving_ = f.moved_tiles > 0 && f.over_lossless_budget;
  stats_.cached_tiles += n_loads;
  stats_.refreshed_tiles += f.refresh.size() - f.put_back - f.progressive_sent;
  stats_.progressive_tiles += f.progressive_sent;
  stats_.lossless_us_total += (f.t1 - f.t0) + (t3 - f.t2);
  rough_waiting_ = false;
  if (f.text) {
    for (size_t idx = 0; idx < f.n_tiles && !rough_waiting_; ++idx) {
      rough_waiting_ = ledger_.RoughWaiting(idx) && !InArea(idx);
    }
  }
}

// The picture that just went out, decoded as the viewer decodes it: each
// 16 x 16 cell of finished (or poor) video the viewer took from it is
// finished if it is within text_video_min_db of the screen, poor otherwise
// (and taken again, see PlanRetakes).
void HostSession::MeasureVideo(Frame& f) {
  if (!mirror_ || !f.picture_coded) return;
  if (!f.text) {
    mirror_->Lose();  // (the pictures of normal mode are not followed)
    return;
  }
  const bool decoded = mirror_->Decode(*f.mirrored, f.in.capture_time_us, [&](const ImageView& picture) {
    const std::vector<uint8_t> cells = MarkTaken(f);
    const int32_t cell_cols = (width_ + kCellSize - 1) / kCellSize;
    for (size_t k = 0; k < cells.size(); ++k) {
      if (!cells[k]) continue;
      const auto cx = static_cast<int32_t>(k % static_cast<size_t>(cell_cols));
      const auto cy = static_cast<int32_t>(k / static_cast<size_t>(cell_cols));
      const Rect cell{cx * kCellSize, cy * kCellSize, std::min(kCellSize, std::min(width_, picture.width) - cx * kCellSize),
                      std::min(kCellSize, std::min(height_, picture.height) - cy * kCellSize)};
      if (cell.w > 0 && cell.h > 0) JudgeCell(f, picture, cell);
    }
  });
  if (!decoded) ledger_.GiveUpPoor();
}

// The pixels the viewer takes from this frame's picture, in taken_: all of
// it for a keyframe, else the rects listed (the lossless ones painted over
// them are exact: they are not looked at). Returns, per cell, whether any
// pixel of it is taken.
std::vector<uint8_t> HostSession::MarkTaken(const Frame& f) {
  const int32_t cell_cols = (width_ + kCellSize - 1) / kCellSize, cell_rows = (height_ + kCellSize - 1) / kCellSize;
  taken_.assign(static_cast<size_t>(width_) * height_, f.key ? 1 : 0);
  std::vector<uint8_t> cells(static_cast<size_t>(cell_cols) * cell_rows, f.key ? 1 : 0);
  if (f.key) return cells;
  for (const auto& t : f.region_tiles) {
    const Rect& r = t.rect;
    for (int32_t y = r.y; y < r.bottom(); ++y) {
      std::memset(taken_.data() + static_cast<size_t>(y) * width_ + r.x, 1, static_cast<size_t>(r.w));
    }
    for (int32_t cy = r.y / kCellSize; cy <= (r.bottom() - 1) / kCellSize; ++cy) {
      for (int32_t cx = r.x / kCellSize; cx <= (r.right() - 1) / kCellSize; ++cx) {
        cells[static_cast<size_t>(cy) * cell_cols + cx] = 1;
      }
    }
  }
  return cells;
}

// One cell of what the viewer took from `picture`: its finished or poor
// video pixels against the screen. Close enough, they are finished. Not
// close, they are poor while retakes bring them closer (by kRetakeGainDb)
// and fewer than text_video_retakes were made; otherwise they are as good
// as the encoder makes them here, and finished all the same.
void HostSession::JudgeCell(Frame& f, const ImageView& picture, const Rect& cell) {
  auto video = [&](uint8_t q, uint8_t taken) { return taken && (q == kPxPolished || q == kPxPoor); };
  double se = 0;
  int64_t n = 0;
  for (int32_t y = cell.y; y < cell.bottom(); ++y) {
    const uint8_t* q = ledger_.PixelRow(y);
    const uint8_t* tk = taken_.data() + static_cast<size_t>(y) * width_;
    const uint8_t* a = picture.data + static_cast<size_t>(y) * picture.stride;
    const uint8_t* b = f.in.bgra.data + static_cast<size_t>(y) * f.in.bgra.stride;
    for (int32_t x = cell.x; x < cell.right(); ++x) {
      if (!video(q[x], tk[x])) continue;
      const int d0 = a[x * 4] - b[x * 4], d1 = a[x * 4 + 1] - b[x * 4 + 1], d2 = a[x * 4 + 2] - b[x * 4 + 2];
      se += d0 * d0 + d1 * d1 + d2 * d2;
      ++n;
    }
  }
  if (n == 0) return;
  const double db = se == 0 ? 99.0 : std::min(99.0, 10 * std::log10(255.0 * 255.0 * 3.0 * static_cast<double>(n) / se));
  const bool close = db >= options_.text_video_min_db;
  const int tries = ledger_.retakes(cell.x, cell.y);
  uint8_t& last = ledger_.measured(cell.x, cell.y);
  const bool closer = tries == 0 || db >= last + kRetakeGainDb;
  const bool poor = !close && closer && tries < options_.text_video_retakes;
  last = static_cast<uint8_t>(db);
  if (tries > 0) {
    f.trace.retaken_close += close;
    f.trace.retaken_left += !close && !poor;
  }
  for (int32_t y = cell.y; y < cell.bottom(); ++y) {
    uint8_t* q = ledger_.PixelRow(y);
    const uint8_t* tk = taken_.data() + static_cast<size_t>(y) * width_;
    for (int32_t x = cell.x; x < cell.right(); ++x) {
      if (video(q[x], tk[x])) q[x] = poor ? kPxPoor : kPxPolished;
    }
  }
  if (poor) {
    ++f.trace.poor_cells;
    ledger_.NotePoor();
  }
}

Rect HostSession::TileRect(size_t idx) const {
  const auto col = static_cast<int32_t>(idx % differ_.cols());
  const auto row = static_cast<int32_t>(idx / differ_.cols());
  Rect r{col * kTileSize, row * kTileSize, kTileSize, kTileSize};
  r.w = std::min(r.w, width_ - r.x);
  r.h = std::min(r.h, height_ - r.y);
  return r;
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

bool HostSession::InArea(size_t idx) const {
  return regions_.RegionOf(static_cast<int32_t>(idx % differ_.cols()), static_cast<int32_t>(idx / differ_.cols())) >= 0;
}

uint8_t HostSession::QualityUnder(const Rect& r) const {
  uint8_t q = kExact;
  for (int32_t row = r.y / kTileSize; row <= (r.bottom() - 1) / kTileSize; ++row) {
    for (int32_t col = r.x / kTileSize; col <= (r.right() - 1) / kTileSize; ++col) {
      q = std::min(q, ledger_[static_cast<size_t>(row) * differ_.cols() + col].quality);
    }
  }
  return q;
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
  // The mirror did not follow the pictures of normal mode: it measures
  // again from a keyframe.
  if (on && mirror_ && !mirror_->in_step()) force_keyframe_ = true;
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
                                const std::vector<uint8_t>& changed, uint64_t now_us) {
  regions_.SetLingerUs(options_.text_mode && options_.lossless && options_.text_linger_us > 0
                           ? options_.text_linger_us
                           : options_.region_params.linger_us);
  const auto closed = regions_.Update(busy, changed, now_us);
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
