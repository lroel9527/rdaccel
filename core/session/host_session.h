#pragma once

// Host side of a connection (M0): every captured frame goes through tile
// change detection; frames with no changed tile are skipped entirely, the
// rest are encoded as a whole HEVC picture and sent with the list of changed
// regions. M1 will route text tiles to the lossless layer instead.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "capture/capturer.h"
#include "rdaccel/host.h"
#include "codec/video_codec.h"
#include "proto/messages.h"
#include "tile/tile_diff.h"
#include "transport/transport.h"

namespace rdaccel::session {

using HostStats = rdaccel::HostStats;

class HostSession {
 public:
  using InputFn = std::function<void(const proto::InputEvent&)>;

  HostSession(transport::ITransport* transport, std::unique_ptr<codec::IHevcEncoder> encoder,
              int32_t width, int32_t height);

  // Called with input events from the viewer (on the transport thread).
  void SetInputHandler(InputFn fn);

  // Feed captured frames here (typically the capturer's callback). Returns
  // false if the encoder failed on this frame; `error` explains.
  bool OnFrame(const capture::CapturedFrame& frame, std::string* error = nullptr);

  // Makes the next encoded frame a keyframe.
  void RequestKeyframe() { force_keyframe_ = true; }

  HostStats stats() const;

 private:
  void OnReceive(transport::Channel ch, std::span<const uint8_t> bytes);
  void Send(transport::Channel ch, const proto::Message& m);

  transport::ITransport* transport_;
  std::unique_ptr<codec::IHevcEncoder> encoder_;
  int32_t width_, height_;
  tile::TileDiffer differ_;
  uint64_t next_frame_id_ = 1;
  std::atomic<bool> force_keyframe_{true};

  mutable std::mutex mu_;
  HostStats stats_;
  InputFn on_input_;
  proto::StreamParser input_parser_;
};

// Merges changed tiles into horizontal runs, one rect per run, to keep the
// region list short.
std::vector<Rect> TilesToRegions(const std::vector<tile::TileChange>& changes);

}  // namespace rdaccel::session
