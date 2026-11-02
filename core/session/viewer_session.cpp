#include "session/viewer_session.h"

#include <algorithm>
#include <cstring>

#include "capture/capturer.h"

namespace rdaccel::session {

ViewerSession::ViewerSession(transport::ITransport* transport,
                             std::unique_ptr<codec::IHevcDecoder> decoder)
    : decoder_(std::move(decoder)) {
  if (transport) Attach(transport);
}

void ViewerSession::Attach(transport::ITransport* transport) {
  transport_ = transport;
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

ViewerStats ViewerSession::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

void ViewerSession::SendInput(const proto::InputEvent& ev) {
  if (transport_) transport_->Send(transport::Channel::kInput, proto::Encode(ev));
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
  if (auto* b = std::get_if<proto::FrameBegin>(&m)) {
    begin_ = *b;
    video_.reset();
    lossless_.reset();
    moves_.clear();
    stores_.clear();
    loads_.clear();
    return;
  }
  if (auto* st = std::get_if<proto::CacheStore>(&m)) {
    if (begin_ && st->frame_id == begin_->frame_id) stores_.push_back(*st);
    return;
  }
  if (auto* ld = std::get_if<proto::CacheLoad>(&m)) {
    if (begin_ && ld->frame_id == begin_->frame_id) loads_.push_back(*ld);
    return;
  }
  if (auto* c = std::get_if<proto::CopyRect>(&m)) {
    if (begin_ && c->frame_id == begin_->frame_id) {
      moves_.push_back({{c->src.x + c->dx, c->src.y + c->dy, c->src.w, c->src.h}, c->dx, c->dy});
    }
    return;
  }
  if (auto* v = std::get_if<proto::VideoData>(&m)) {
    if (begin_ && v->frame_id == begin_->frame_id) video_ = std::move(*v);
    return;
  }
  if (auto* l = std::get_if<proto::LosslessData>(&m)) {
    if (begin_ && l->frame_id == begin_->frame_id) lossless_ = std::move(*l);
    return;
  }
  auto* end = std::get_if<proto::FrameEnd>(&m);
  if (!end || !begin_ || end->frame_id != begin_->frame_id || (!video_ && !lossless_ && moves_.empty() && loads_.empty() && stores_.empty())) {
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

  // Order matters: moves read the screen as the previous frame left it, then
  // video regions, then lossless tiles on top.
  const bool needs_screen = !stores_.empty() || !moves_.empty() || !loads_.empty();
  if (needs_screen && !have_keyframe_) ok = false;
  const size_t stride = static_cast<size_t>(width_) * 4;
  auto inside = [&](const Rect& r) {
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.right() <= width_ &&
           r.bottom() <= height_;
  };
  if (ok) {
    for (const auto& st : stores_) {
      if (!inside(st.rect)) {
        ok = false;
        break;
      }
      if (cache_.size() < proto::kTileCacheSlots) cache_.resize(proto::kTileCacheSlots);
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
  if (ok && !moves_.empty()) {
    tile::ApplyMoves(moves_, screen_.data(), width_, height_, scratch_);
    for (const auto& mv : moves_) changed.push_back(mv.dst);
  }
  if (ok) {
    for (const auto& ld : loads_) {
      const CacheSlot* slot = ld.slot < cache_.size() ? &cache_[ld.slot] : nullptr;
      if (!inside(ld.rect) || !slot || slot->w != ld.rect.w || slot->h != ld.rect.h) {
        ok = false;
        break;
      }
      for (int32_t y = 0; y < slot->h; ++y) {
        std::memcpy(screen_.data() + (ld.rect.y + y) * stride + static_cast<size_t>(ld.rect.x) * 4,
                    slot->bgra.data() + static_cast<size_t>(y) * slot->w * 4,
                    static_cast<size_t>(slot->w) * 4);
      }
      changed.push_back(ld.rect);
    }
  }
  if (ok && video_) {
    // Parameter sets come with keyframes only; decoders keep them as state.
    ok = decoder_->Decode(video_->parameter_sets, video_->nal_data, capture_time,
                          [&](const tile::ImageView& img, uint64_t) {
                            if (screen_.empty()) EnsureScreen(img.width, img.height);
                            const int32_t stride = width_ * 4;
                            if (video_->regions.empty()) {
                              full = true;
                              CopyRegion(img, {0, 0, width_, height_}, screen_.data(), stride,
                                         width_, height_);
                            } else {
                              for (const Rect& r : video_->regions) {
                                CopyRegion(img, r, screen_.data(), stride, width_, height_);
                              }
                              changed.insert(changed.end(), video_->regions.begin(),
                                             video_->regions.end());
                            }
                            if (full) have_keyframe_ = true;
                          });
  }
  if (ok && lossless_) {
    if (!have_keyframe_) {
      ok = false;  // nothing to paint on before the first keyframe
    } else {
      ok = lossless_dec_.Decode(lossless_->data, lossless_->reset, width_, height_, tiles_);
      if (ok) {
        for (const auto& t : tiles_) {
          codec::PaintPalette(t, screen_.data(), width_ * 4);
          if (!full) changed.push_back(t.rect);
        }
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
    } else {
      ++stats_.decode_errors;
    }
  }
  begin_.reset();
  video_.reset();
  lossless_.reset();
  moves_.clear();
  stores_.clear();
  loads_.clear();
}

}  // namespace rdaccel::session
