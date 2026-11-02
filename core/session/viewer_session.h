#pragma once

// Viewer side of a connection (M0): reassembles frames from the video
// channel, decodes them, and hands BGRA pictures to the display.

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "codec/video_codec.h"
#include "proto/messages.h"
#include "transport/transport.h"

namespace rdaccel::session {

struct ViewerStats {
  uint64_t frames_decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t bytes_received = 0;
  uint64_t decode_us_total = 0;
};

class ViewerSession {
 public:
  // `on_frame` receives each decoded picture plus the regions that changed
  // (empty = whole screen) on the transport thread.
  using FrameFn = std::function<void(const tile::ImageView& bgra, const std::vector<Rect>& regions,
                                     uint64_t frame_id, uint64_t capture_time_us)>;
  using HelloFn = std::function<void(const proto::Hello&)>;

  ViewerSession(transport::ITransport* transport, std::unique_ptr<codec::IHevcDecoder> decoder);

  void SetHelloHandler(HelloFn fn);
  void SetFrameHandler(FrameFn fn);
  void SendInput(const proto::InputEvent& ev);

  ViewerStats stats() const;

  // Feeds already-framed protocol bytes directly (used by file playback).
  void FeedVideoBytes(std::span<const uint8_t> bytes);

 private:
  void OnReceive(transport::Channel ch, std::span<const uint8_t> bytes);
  void Handle(proto::Message&& m);

  transport::ITransport* transport_;
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
