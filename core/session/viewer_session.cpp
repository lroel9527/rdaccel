#include "session/viewer_session.h"

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

void ViewerSession::Handle(proto::Message&& m) {
  if (auto* hello = std::get_if<proto::Hello>(&m)) {
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
    return;
  }
  if (auto* v = std::get_if<proto::VideoData>(&m)) {
    if (begin_ && v->frame_id == begin_->frame_id) video_ = std::move(*v);
    return;
  }
  auto* end = std::get_if<proto::FrameEnd>(&m);
  if (!end || !begin_ || end->frame_id != begin_->frame_id || !video_) return;

  // Parameter sets come with keyframes only; decoders keep them as state.
  const auto& ps = video_->parameter_sets;

  FrameFn fn;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fn = on_frame_;
  }
  const uint64_t t0 = capture::MonotonicMicros();
  const uint64_t frame_id = begin_->frame_id;
  const uint64_t capture_time = begin_->capture_time_us;
  const auto& regions = video_->regions;
  const bool ok = decoder_->Decode(ps, video_->nal_data, capture_time,
                                   [&](const tile::ImageView& img, uint64_t) {
                                     if (fn) fn(img, regions, frame_id, capture_time);
                                   });
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
}

}  // namespace rdaccel::session
