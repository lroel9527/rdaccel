#include "session/host_session.h"

#include <algorithm>

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
                         int32_t height)
    : transport_(transport), encoder_(std::move(encoder)), width_(width), height_(height) {
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

bool HostSession::OnFrame(const capture::CapturedFrame& frame, std::string* error) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.frames_captured;
  }
  const bool key = force_keyframe_.exchange(false);
  auto changes = differ_.Diff(frame.bgra, frame.dirty);
  if (changes.empty() && !key) {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.frames_skipped;
    return true;
  }

  const uint64_t t0 = capture::MonotonicMicros();
  auto au = encoder_->Encode(frame.bgra, frame.capture_time_us, key);
  const uint64_t t1 = capture::MonotonicMicros();
  if (!au) {
    // The encoder dropped or failed this frame; make sure the next one fully
    // refreshes the viewer.
    force_keyframe_ = true;
    differ_.Reset();
    // An empty error means the encoder chose to drop the frame, which is
    // not a failure.
    const std::string& why = encoder_->last_error();
    if (why.empty()) return true;
    if (error) *error = why;
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.encode_errors;
    return false;
  }

  const uint64_t id = next_frame_id_++;
  proto::VideoData vd;
  vd.frame_id = id;
  vd.keyframe = au->keyframe;
  vd.parameter_sets = std::move(au->parameter_sets);
  vd.nal_data = std::move(au->nal_data);
  if (!au->keyframe) vd.regions = TilesToRegions(changes);  // keyframe = whole screen

  std::vector<uint8_t> bytes;
  proto::EncodeTo(proto::FrameBegin{id, frame.capture_time_us}, bytes);
  proto::EncodeTo(vd, bytes);
  proto::EncodeTo(proto::FrameEnd{id}, bytes);
  transport_->Send(transport::Channel::kVideo, bytes);

  std::lock_guard<std::mutex> lock(mu_);
  ++stats_.frames_encoded;
  if (vd.keyframe) ++stats_.keyframes;
  stats_.bytes_sent += bytes.size();
  stats_.encode_us_total += t1 - t0;
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
