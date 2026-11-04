#pragma once

// Viewer side of a connection: reassembles frames from the video channel,
// decodes the video and lossless layers into one composed screen, and hands
// that BGRA picture to the display.

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codec/lossless.h"
#include "codec/video_codec.h"
#include "rdacore/viewer.h"
#include "proto/messages.h"
#include "session/cursor_channel.h"
#include "tile/motion.h"
#include "transport/transport.h"

namespace rdacore::session {

using ViewerStats = rdacore::ViewerStats;

class ViewerSession {
 public:
  // `on_frame` receives the composed screen after each frame plus the regions
  // that changed (empty = whole screen). Bytes from a transport are decoded
  // on the session's own thread, so a slow decode (or a slow `on_frame`)
  // holds up neither the transport nor the cursor: cursor messages are
  // handled at once on the receiving thread.
  using FrameFn = std::function<void(const tile::ImageView& bgra, const std::vector<Rect>& regions,
                                     uint64_t frame_id, uint64_t capture_time_us)>;
  using HelloFn = std::function<void(const proto::Hello&)>;

  // `transport` may be null (file playback, or attach later with Attach()).
  ViewerSession(transport::ITransport* transport, std::unique_ptr<codec::IHevcDecoder> decoder);
  ~ViewerSession();
  ViewerSession(const ViewerSession&) = delete;
  ViewerSession& operator=(const ViewerSession&) = delete;
  // Stops the decode thread (bytes still queued are dropped, later ones
  // ignored); call it before the transport goes away, as decoding can send
  // keyframe requests.
  void Stop();

  // Starts receiving from `transport`. Lets callers install handlers first so
  // the first frames are not lost. With `own_receiver` false the session only
  // sends on it, and the owner passes received bytes to OnReceive().
  void Attach(transport::ITransport* transport, bool own_receiver = true);
  void OnReceive(transport::Channel ch, std::span<const uint8_t> bytes);

  void SetHelloHandler(HelloFn fn);
  void SetFrameHandler(FrameFn fn);
  using CursorFn = std::function<void(const CursorState&)>;
  void SetCursorHandler(CursorFn fn);
  // Creates decoders for video region streams (see proto::VideoData::stream).
  // Without one, region pictures count as decode errors.
  using DecoderFactory = std::function<std::unique_ptr<codec::IHevcDecoder>()>;
  void SetDecoderFactory(DecoderFactory fn) { decoder_factory_ = std::move(fn); }
  void SendInput(const proto::InputEvent& ev);
  // Sends a KeyframeRequest (no-op without a transport).
  void RequestKeyframe();

  ViewerStats stats() const;

  // Feeds already-framed protocol bytes directly (used by file playback).
  void FeedVideoBytes(std::span<const uint8_t> bytes);

 private:
  void Handle(proto::Message&& m);
  void EnsureScreen(int32_t w, int32_t h);
  void DecodeLoop();
  void Parse(transport::Channel ch, std::span<const uint8_t> bytes);

  // Video and control bytes waiting for the decode thread (control goes the
  // same way so that a Hello stays ordered with the frames around it).
  std::mutex q_mu_;
  std::condition_variable q_cv_;
  std::deque<std::pair<transport::Channel, std::vector<uint8_t>>> queue_;
  bool stop_ = false;
  std::thread worker_;  // started by the first queued bytes

  transport::ITransport* transport_ = nullptr;
  std::unique_ptr<codec::IHevcDecoder> decoder_;
  DecoderFactory decoder_factory_;
  struct RegionDecoder {
    std::unique_ptr<codec::IHevcDecoder> dec;
    uint64_t last_used = 0;  // frame id
  };
  static constexpr size_t kMaxRegionDecoders = 4;
  std::unordered_map<uint8_t, RegionDecoder> region_dec_;
  bool DecodeRegion(const proto::VideoData& v, uint64_t frame_id, uint64_t capture_time,
                    std::vector<Rect>& changed);
  proto::StreamParser video_parser_;
  proto::StreamParser control_parser_;
  proto::StreamParser cursor_parser_;
  CursorReceiver cursor_;
  CursorFn on_cursor_;  // guarded by mu_

  // Frame being assembled.
  std::optional<proto::FrameBegin> begin_;
  std::vector<proto::VideoData> videos_;  // full-screen stream first, then areas
  std::optional<proto::LosslessData> lossless_;
  codec::RegionOps ops_;           // decoded from lossless_, reused
  std::vector<tile::Move> moves_;  // ops_.moves in the form ApplyMoves takes
  struct CacheSlot {
    int32_t w = 0, h = 0;
    std::vector<uint8_t> bgra;
  };
  std::vector<CacheSlot> cache_;  // proto::kTileCacheSlots, filled on demand
  std::vector<uint8_t> scratch_;

  // The composed screen: video regions and lossless tiles are applied to it
  // in frame order. Only touched on the decode thread (or the caller's,
  // for FeedVideoBytes).
  int32_t width_ = 0, height_ = 0;
  std::vector<uint8_t> screen_;
  bool have_keyframe_ = false;  // screen_ holds a whole picture
  uint64_t last_shown_ = 0;        // frame id
  uint64_t last_request_us_ = 0;   // automatic keyframe requests, rate limited
  codec::LosslessDecoder lossless_dec_;

  mutable std::mutex mu_;
  ViewerStats stats_;
  FrameFn on_frame_;
  HelloFn on_hello_;
};

}  // namespace rdacore::session
