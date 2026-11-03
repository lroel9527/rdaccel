#include "session/viewer_session.h"

#include <algorithm>
#include <cstring>

#include "capture/capturer.h"

namespace rdacore::session {

ViewerSession::ViewerSession(transport::ITransport* transport,
                             std::unique_ptr<codec::IHevcDecoder> decoder)
    : decoder_(std::move(decoder)) {
  if (transport) Attach(transport);
}

void ViewerSession::Attach(transport::ITransport* transport, bool own_receiver) {
  transport_ = transport;
  if (!own_receiver) return;
  transport_->SetReceiver(
      [this](transport::Channel ch, std::span<const uint8_t> bytes) { OnReceive(ch, bytes); });
}

void ViewerSession::SetHelloHandler(HelloFn fn) {
  std::lock_guard<std::mutex> lock(mu_);
  on_hello_ = std::move(fn);
}

void ViewerSession::SetFrameHandler(FrameFn fn) {
  std::lock_guard<std::mutex> lock(mu_);
  on_frame_ = std::move(fn);
}

void ViewerSession::SetCursorHandler(CursorFn fn) {
  std::lock_guard<std::mutex> lock(mu_);
  on_cursor_ = std::move(fn);
}

ViewerStats ViewerSession::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

void ViewerSession::SendInput(const proto::InputEvent& ev) {
  if (transport_) transport_->Send(transport::Channel::kInput, proto::Encode(ev));
}

void ViewerSession::RequestKeyframe() {
  if (!transport_) return;
  uint64_t last;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.keyframe_requests;
    last = last_shown_;
  }
  transport_->Send(transport::Channel::kControl, proto::Encode(proto::KeyframeRequest{last}));
}

void ViewerSession::OnReceive(transport::Channel ch, std::span<const uint8_t> bytes) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.bytes_received += bytes.size();
  }
  proto::StreamParser* parser = nullptr;
  if (ch == transport::Channel::kVideo) {
    parser = &video_parser_;
  } else if (ch == transport::Channel::kControl) {
    parser = &control_parser_;
  } else if (ch == transport::Channel::kCursor) {
    parser = &cursor_parser_;
  } else {
    return;
  }
  parser->Append(bytes);
  while (auto m = parser->Next()) Handle(std::move(*m));
}

void ViewerSession::FeedVideoBytes(std::span<const uint8_t> bytes) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.bytes_received += bytes.size();
  }
  video_parser_.Append(bytes);
  while (auto m = video_parser_.Next()) Handle(std::move(*m));
}

void ViewerSession::EnsureScreen(int32_t w, int32_t h) {
  if (w == width_ && h == height_ && !screen_.empty()) return;
  if (width_ > 0 && (w != width_ || h != height_)) {
    // A new stream at another size: everything decoded so far is stale.
    if (decoder_factory_) {
      if (auto dec = decoder_factory_()) decoder_ = std::move(dec);
    }
    region_dec_.clear();
    cache_.clear();
    begin_.reset();
    videos_.clear();
    lossless_.reset();
  }
  width_ = w;
  height_ = h;
  screen_.assign(static_cast<size_t>(w) * h * 4, 0);
  have_keyframe_ = false;
}

namespace {

void CopyRegion(const tile::ImageView& src, Rect r, uint8_t* dst, int32_t dst_stride, int32_t w,
                int32_t h) {
  const int32_t x1 = std::min({r.right(), w, src.width});
  const int32_t y1 = std::min({r.bottom(), h, src.height});
  r.x = std::max(r.x, 0);
  r.y = std::max(r.y, 0);
  r.w = x1 - r.x;
  r.h = y1 - r.y;
  if (r.empty()) return;
  const size_t row = static_cast<size_t>(r.w) * 4;
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * dst_stride + static_cast<size_t>(r.x) * 4,
                src.data + static_cast<size_t>(y) * src.stride + static_cast<size_t>(r.x) * 4, row);
  }
}

}  // namespace

void ViewerSession::Handle(proto::Message&& m) {
  if (auto* hello = std::get_if<proto::Hello>(&m)) {
    EnsureScreen(static_cast<int32_t>(hello->width), static_cast<int32_t>(hello->height));
    HelloFn fn;
    {
      std::lock_guard<std::mutex> lock(mu_);
      fn = on_hello_;
    }
    if (fn) fn(*hello);
    return;
  }
  if (auto* cs = std::get_if<proto::CursorShape>(&m)) {
    cursor_.OnShape(std::move(*cs));
    return;
  }
  if (auto* cp = std::get_if<proto::CursorPos>(&m)) {
    CursorState st;
    cursor_.OnPos(*cp, st);
    CursorFn fn;
    {
      std::lock_guard<std::mutex> lock(mu_);
      fn = on_cursor_;
    }
    if (fn) fn(st);
    return;
  }
  if (auto* b = std::get_if<proto::FrameBegin>(&m)) {
    begin_ = *b;
    videos_.clear();
    lossless_.reset();
    return;
  }
  if (auto* v = std::get_if<proto::VideoData>(&m)) {
    if (begin_ && v->frame_id == begin_->frame_id) {
      // The full-screen picture is applied before any area picture.
      if (v->stream == 0) {
        videos_.insert(videos_.begin(), std::move(*v));
      } else {
        videos_.push_back(std::move(*v));
      }
    }
    return;
  }
  if (auto* l = std::get_if<proto::LosslessData>(&m)) {
    if (begin_ && l->frame_id == begin_->frame_id) lossless_ = std::move(*l);
    return;
  }
  auto* end = std::get_if<proto::FrameEnd>(&m);
  if (!end || !begin_ || end->frame_id != begin_->frame_id || (videos_.empty() && !lossless_)) {
    return;
  }

  FrameFn fn;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fn = on_frame_;
  }
  const uint64_t t0 = capture::MonotonicMicros();
  const uint64_t frame_id = begin_->frame_id;
  const uint64_t capture_time = begin_->capture_time_us;
  bool ok = true;
  bool full = false;
  std::vector<Rect> changed;

  // Order matters: stores and moves read the screen as the previous frame
  // left it, then cache loads, then video regions, then lossless tiles on top.
  ops_ = {};
  if (lossless_) {
    ok = width_ > 0 &&
         lossless_dec_.Decode(lossless_->data, lossless_->reset, width_, height_,
                              proto::kTileCacheSlots, ops_);
  }
  const bool needs_screen = !ops_.stores.empty() || !ops_.moves.empty() || !ops_.loads.empty();
  if (ok && needs_screen && !have_keyframe_) ok = false;
  const size_t stride = static_cast<size_t>(width_) * 4;
  if (ok) {
    if (!ops_.stores.empty() && cache_.size() < proto::kTileCacheSlots) {
      cache_.resize(proto::kTileCacheSlots);
    }
    for (const auto& st : ops_.stores) {
      CacheSlot& slot = cache_[st.slot];
      slot.w = st.rect.w;
      slot.h = st.rect.h;
      slot.bgra.resize(static_cast<size_t>(slot.w) * slot.h * 4);
      for (int32_t y = 0; y < slot.h; ++y) {
        std::memcpy(slot.bgra.data() + static_cast<size_t>(y) * slot.w * 4,
                    screen_.data() + (st.rect.y + y) * stride + static_cast<size_t>(st.rect.x) * 4,
                    static_cast<size_t>(slot.w) * 4);
      }
    }
  }
  if (ok && !ops_.moves.empty()) {
    moves_.clear();
    for (const auto& mv : ops_.moves) moves_.push_back({mv.dst, mv.dx, mv.dy});
    tile::ApplyMoves(moves_, screen_.data(), width_, height_, scratch_);
    for (const auto& mv : moves_) changed.push_back(mv.dst);
  }
  if (ok) {
    for (const auto& ld : ops_.loads) {
      const CacheSlot* slot = ld.slot < cache_.size() ? &cache_[ld.slot] : nullptr;
      if (!slot || static_cast<int64_t>(ld.sx) + ld.rect.w > slot->w ||
          static_cast<int64_t>(ld.sy) + ld.rect.h > slot->h) {
        ok = false;  // a slot that was never filled this large
        break;
      }
      for (int32_t y = 0; y < ld.rect.h; ++y) {
        std::memcpy(screen_.data() + (ld.rect.y + y) * stride + static_cast<size_t>(ld.rect.x) * 4,
                    slot->bgra.data() + ((ld.sy + y) * static_cast<size_t>(slot->w) + ld.sx) * 4,
                    static_cast<size_t>(ld.rect.w) * 4);
      }
      changed.push_back(ld.rect);
    }
  }
  const proto::VideoData* video =
      !videos_.empty() && videos_.front().stream == 0 ? &videos_.front() : nullptr;
  if (ok && video) {
    // Parameter sets come with keyframes only; decoders keep them as state.
    ok = decoder_->Decode(video->parameter_sets, video->nal_data, capture_time,
                          [&](const tile::ImageView& img, uint64_t) {
                            if (screen_.empty()) EnsureScreen(img.width, img.height);
                            const int32_t stride = width_ * 4;
                            if (video->regions.empty()) {
                              full = true;
                              CopyRegion(img, {0, 0, width_, height_}, screen_.data(), stride,
                                         width_, height_);
                            } else {
                              for (const Rect& r : video->regions) {
                                CopyRegion(img, r, screen_.data(), stride, width_, height_);
                              }
                              changed.insert(changed.end(), video->regions.begin(),
                                             video->regions.end());
                            }
                            if (full) have_keyframe_ = true;
                          });
  }
  for (const auto& v : videos_) {
    if (!ok) break;
    if (v.stream == 0) continue;
    ok = have_keyframe_ && DecodeRegion(v, frame_id, capture_time, changed);
  }
  if (ok && !ops_.tiles.empty()) {
    if (!have_keyframe_) {
      ok = false;  // nothing to paint on before the first keyframe
    } else {
      for (const auto& t : ops_.tiles) {
        codec::PaintPalette(t, screen_.data(), width_ * 4);
        if (!full) changed.push_back(t.rect);
      }
    }
  }
  if (ok && have_keyframe_ && fn) {
    if (full) changed.clear();
    fn({screen_.data(), width_, height_, width_ * 4}, changed, frame_id, capture_time);
  }
  const uint64_t t1 = capture::MonotonicMicros();
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (ok) {
      ++stats_.frames_decoded;
      stats_.decode_us_total += t1 - t0;
      last_shown_ = frame_id;
    } else {
      ++stats_.decode_errors;
    }
  }
  // Undecodable: ask for a refresh, but not for every frame still on the way.
  if (!ok && transport_ && (last_request_us_ == 0 || t1 - last_request_us_ >= 250'000)) {
    last_request_us_ = t1;
    RequestKeyframe();
  }
  begin_.reset();
  videos_.clear();
  lossless_.reset();
}

bool ViewerSession::DecodeRegion(const proto::VideoData& v, uint64_t frame_id,
                                 uint64_t capture_time, std::vector<Rect>& changed) {
  auto it = region_dec_.find(v.stream);
  if (v.keyframe) {
    // A keyframe starts the stream over: the area may have a new size.
    if (!decoder_factory_) return false;
    auto dec = decoder_factory_();
    if (!dec) return false;
    if (region_dec_.size() >= kMaxRegionDecoders && it == region_dec_.end()) {
      auto oldest = std::min_element(region_dec_.begin(), region_dec_.end(),
                                     [](const auto& a, const auto& b) {
                                       return a.second.last_used < b.second.last_used;
                                     });
      region_dec_.erase(oldest);
    }
    RegionDecoder& rd = region_dec_[v.stream];
    rd.dec = std::move(dec);
    it = region_dec_.find(v.stream);
  }
  if (it == region_dec_.end()) return false;  // joined the stream mid-way
  it->second.last_used = frame_id;
  const size_t stride = static_cast<size_t>(width_) * 4;
  return it->second.dec->Decode(
      v.parameter_sets, v.nal_data, capture_time, [&](const tile::ImageView& img, uint64_t) {
        Rect r{v.x, v.y, img.width, img.height};
        // Clip to the screen.
        const int32_t x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
        const int32_t x1 = std::min(r.right(), width_), y1 = std::min(r.bottom(), height_);
        if (x1 <= x0 || y1 <= y0) return;
        for (int32_t y = y0; y < y1; ++y) {
          std::memcpy(screen_.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x0) * 4,
                      img.data + static_cast<size_t>(y - r.y) * img.stride +
                          static_cast<size_t>(x0 - r.x) * 4,
                      static_cast<size_t>(x1 - x0) * 4);
        }
        changed.push_back({x0, y0, x1 - x0, y1 - y0});
      });
}

}  // namespace rdacore::session
