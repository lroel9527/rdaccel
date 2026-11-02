#pragma once

// Viewer side of a connection (M0): reassembles frames from the video
// channel, decodes them, and hands BGRA pictures to the display.

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "codec/video_codec.h"
#include "rdaccel/viewer.h"
#include "proto/messages.h"
#include "transport/transport.h"

namespace rdaccel::session {

using ViewerStats = rdaccel::ViewerStats;

class ViewerSession {
 public:
  // `on_frame` receives each decoded picture plus the regions that changed
  // (empty = whole screen) on the transport thread.
  using FrameFn = std::function<void(const tile::ImageView& bgra, const std::vector<Rect>& regions,
                                     uint64_t frame_id, uint64_t capture_time_us)>;
  using HelloFn = std::function<void(const proto::Hello&)>;

  // `transport` may be null (file playback, or attach later with Attach()).
  ViewerSession(transport::ITransport* transport, std::unique_ptr<codec::IHevcDecoder> decoder);

  // Starts receiving from `transport`. Lets callers install handlers first so
  // the first frames are not lost.
  void Attach(transport::ITransport* transport);

  void SetHelloHandler(HelloFn fn);
  void SetFrameHandler(FrameFn fn);
  void SendInput(const proto::InputEvent& ev);

  ViewerStats stats() const;

  // Feeds already-framed protocol bytes directly (used by file playback).
  void FeedVideoBytes(std::span<const uint8_t> bytes);

 private:
  void OnReceive(transport::Channel ch, std::span<const uint8_t> bytes);
  void Handle(proto::Message&& m);

  transport::ITransport* transport_ = nullptr;
  std::unique_ptr<codec::IHevcDecoder> decoder_;
  proto::StreamParser video_parser_;
  proto::StreamParser control_parser_;

  // Frame being assembled.
  std::optional<proto::FrameBegin> begin_;
  std::optional<proto::VideoData> video_;

  mutable std::mutex mu_;
  ViewerStats stats_;
  FrameFn on_frame_;
  HelloFn on_hello_;
};

}  // namespace rdaccel::session
