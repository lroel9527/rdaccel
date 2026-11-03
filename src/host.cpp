#include "rdaccel/host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "capture/capturer.h"
#include "platform.h"
#include "session/cursor_channel.h"
#include "session/host_session.h"
#include "session/rate_control.h"

namespace rdaccel {

struct Host::Impl {
  HostConfig config;

  // Created on the first frame, when the resolution is known. Never replaced,
  // so readers only need the atomic pointer.
  std::unique_ptr<session::HostSession> session;
  std::atomic<session::HostSession*> session_ptr{nullptr};
  std::unique_ptr<capture::ICapturer> capturer;
  // Declared after `session`, so it is destroyed first: its destructor stops
  // the receive thread that calls into the session.
  std::unique_ptr<Transport> transport;
  // After `transport`, so it goes first: it sends on it.
  std::unique_ptr<session::CursorSender> cursor;

  // System cursor polling (screen capture mode).
  std::thread cursor_thread;
  std::atomic<bool> cursor_running{false};
  std::atomic<int32_t> frame_w{0}, frame_h{0};

  void PollCursor(std::unique_ptr<capture::ICursorSource> src) {
    const int hz = std::clamp(config.cursor_poll_hz, 1, 1000);
    const auto period = std::chrono::microseconds(1'000'000 / hz);
    auto next = std::chrono::steady_clock::now();
    capture::CursorSample s;
    CursorImage original;      // as the source reported it
    double sent_factor = 0;    // scale of the image last sent (0 = none sent)
    while (cursor_running) {
      next += period;
      std::this_thread::sleep_until(next);
      if (!src->Poll(s)) continue;
      const int32_t w = frame_w, h = frame_h;
      if (s.image_changed) {
        original = std::move(s.image);
        sent_factor = 0;
      }
      if (w <= 0 || s.screen_width <= 0 || s.screen_height <= 0) continue;  // no frame yet
      // The image follows the captured frame's scale (a 4K or Retina screen
      // captured at 1080p gets a cursor of the matching, smaller size).
      const double factor = w / (s.screen_width * std::max(s.image_scale, 0.01));
      if (original.width > 0 && std::abs(factor - sent_factor) > 1e-3) {
        cursor->SetImage(std::abs(factor - 1) < 0.02 ? original
                                                     : session::ScaleCursorImage(original, factor));
        sent_factor = factor;
      }
      const auto x = static_cast<int32_t>(std::lround(s.x * w / s.screen_width));
      const auto y = static_cast<int32_t>(std::lround(s.y * h / s.screen_height));
      cursor->SetPosition(x, y, s.visible, capture::MonotonicMicros());
    }
  }

  // Static screens. A source may stop delivering frames while nothing
  // changes (ScreenCaptureKit does; so may an application). A keyframe the
  // viewer asks for, or the changes of a frame skipped for congestion, would
  // then wait for the next change. This thread re-submits the screen after
  // `kIdleResubmitUs` without frames.
  static constexpr uint64_t kIdleResubmitUs = 100'000;
  std::thread idle_thread;
  std::mutex idle_mu;
  std::condition_variable idle_cv;
  bool idle_stop = false;
  std::atomic<uint64_t> last_frame_us{0};
  // A copy of the newest frame skipped for congestion (under submit_mu).
  std::vector<uint8_t> skipped_px;
  uint64_t frames_resubmitted = 0;  // under mu

  void IdleLoop() {
    std::unique_lock<std::mutex> lock(idle_mu);
    while (!idle_cv.wait_for(lock, std::chrono::milliseconds(25), [&] { return idle_stop; })) {
      auto* s = session_ptr.load();
      if (!s || failed) continue;
      const uint64_t now = capture::MonotonicMicros();
      if (now - last_frame_us.load() < kIdleResubmitUs) continue;
      // Under submit_mu throughout, so no newer frame can go in between.
      std::lock_guard<std::mutex> l(submit_mu);
      std::vector<uint8_t> px;
      if (!skipped_px.empty()) {
        px = std::move(skipped_px);
        skipped_px.clear();
      } else if (s->keyframe_pending()) {
        px = s->last_frame();
      } else {
        continue;
      }
      capture::CapturedFrame f;
      f.bgra = {px.data(), s->width(), s->height(), s->width() * 4};
      f.capture_time_us = now;
      {
        std::lock_guard<std::mutex> m(mu);
        ++frames_resubmitted;
      }
      OnFrameLocked(f, nullptr);
    }
  }

  std::mutex submit_mu;  // serializes frames (screen capture vs SubmitFrame)
  std::atomic<bool> failed{false};  // encoder creation failed; drop further frames
  bool size_error_reported = false;
  uint64_t encode_errors_reported = 0;
  int32_t width = 0, height = 0;

  std::unique_ptr<session::RateController> rate;  // only touched under submit_mu

  mutable std::mutex mu;  // guards the fields below
  uint64_t frames_dropped_congestion = 0;
  int64_t target_bitrate_bps = 0;
  TransportStats net;
  std::string encoder_name;
  InputHandler on_input;
  ErrorHandler on_error;

  void ReportError(const std::string& msg) {
    ErrorHandler fn;
    {
      std::lock_guard<std::mutex> lock(mu);
      fn = on_error;
    }
    if (fn) fn(msg);
  }

  bool OnFrame(const capture::CapturedFrame& f, std::string* error) {
    std::lock_guard<std::mutex> lock(submit_mu);
    return OnFrameLocked(f, error);
  }

  bool OnFrameLocked(const capture::CapturedFrame& f, std::string* error) {
    last_frame_us = capture::MonotonicMicros();
    if (failed) {
      if (error) *error = "host stopped after an earlier encoder error";
      return false;
    }
    if (!session) {
      codec::EncoderConfig cfg{f.bgra.width, f.bgra.height, config.fps, config.bitrate_bps,
                               config.min_qp};
      std::string err;
      auto enc = platform::CreateHevcEncoder(cfg, config.encoder, &err);
      if (!enc) {
        failed = true;
        if (error) *error = err;
        ReportError(err);  // also reaches apps that only use screen capture
        return false;
      }
      {
        std::lock_guard<std::mutex> l(mu);
        encoder_name = enc->name();
      }
      width = f.bgra.width;
      height = f.bgra.height;
      frame_w = width;
      frame_h = height;
      session::HostOptions opts;
      opts.lossless = config.lossless;
      opts.lossless_max_colors = config.lossless_max_colors;
      opts.lossless_max_colors_hot = config.lossless_max_colors_hot;
      opts.detect_moves = config.detect_moves;
      opts.refine_frames = config.refine_frames;
      opts.tile_cache = config.tile_cache;
      opts.refresh = config.refresh;
      opts.progressive = config.progressive;
      if (config.video_regions) {
        const std::string prefer = config.encoder;
        opts.region_encoder_factory = [prefer](const codec::EncoderConfig& c, std::string* e) {
          return platform::CreateHevcEncoder(c, prefer, e);
        };
      }
      opts.fps = config.fps;
      opts.bitrate_bps = config.bitrate_bps;
      opts.region_min_qp = config.video_region_min_qp;
      opts.region_bitrate_bps = config.video_region_bitrate_bps;
      session = std::make_unique<session::HostSession>(transport.get(), std::move(enc), width,
                                                       height, opts);
      session->SetInputHandler([this](const InputEvent& ev) {
        InputHandler fn;
        {
          std::lock_guard<std::mutex> l(mu);
          fn = on_input;
        }
        if (fn) fn(ev);
      });
      session_ptr = session.get();
    } else if (f.bgra.width != width || f.bgra.height != height) {
      const std::string err = "frame size changed from " + std::to_string(width) + "x" +
                              std::to_string(height) + " to " + std::to_string(f.bgra.width) +
                              "x" + std::to_string(f.bgra.height) + " (not supported yet)";
      if (error) *error = err;
      if (!size_error_reported) {
        size_error_reported = true;
        ReportError(err);
      }
      return false;
    }
    // Adaptive bitrate: follow what the transport measures.
    TransportStats st;
    if (config.adaptive_bitrate && transport->GetStats(&st)) {
      if (!rate) {
        rate = std::make_unique<session::RateController>(
            config.bitrate_bps, std::min(config.min_bitrate_bps, config.bitrate_bps), config.max_queue_ms);
      }
      const auto d = rate->Update(capture::MonotonicMicros(), st);
      if (d.new_target_bps > 0) session->SetBitrate(d.new_target_bps);
      {
        std::lock_guard<std::mutex> l(mu);
        net = st;
        target_bitrate_bps = rate->target_bps();
        if (d.skip_frame) ++frames_dropped_congestion;
      }
      // The next frame that goes through carries all changes since; keep
      // this one in case no next frame comes.
      if (d.skip_frame) {
        skipped_px.resize(static_cast<size_t>(width) * height * 4);
        for (int32_t y = 0; y < height; ++y) {
          std::memcpy(skipped_px.data() + static_cast<size_t>(y) * width * 4,
                      f.bgra.data + static_cast<size_t>(y) * f.bgra.stride,
                      static_cast<size_t>(width) * 4);
        }
        return true;
      }
      skipped_px.clear();
    }
    std::string err;
    if (!session->OnFrame(f, &err)) {
      if (error) *error = err;
      // Report the first failure and then one per second's worth of frames,
      // so a broken encoder is visible without flooding the handler.
      if (encode_errors_reported++ % static_cast<uint64_t>(std::max(1, config.fps)) == 0) {
        ReportError("encode failed: " + err);
      }
      return false;
    }
    return true;
  }
};

Host::Host(std::unique_ptr<Transport> transport, HostConfig config)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->transport = std::move(transport);
  impl_->cursor = std::make_unique<session::CursorSender>(impl_->transport.get());
  impl_->idle_thread = std::thread([impl = impl_.get()] { impl->IdleLoop(); });
}

Host::~Host() {
  StopScreenCapture();
  {
    std::lock_guard<std::mutex> lock(impl_->idle_mu);
    impl_->idle_stop = true;
  }
  impl_->idle_cv.notify_all();
  impl_->idle_thread.join();
  impl_->cursor.reset();
  // Stop the receive thread before the session goes away.
  impl_->transport.reset();
}

void Host::SetInputHandler(InputHandler fn) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->on_input = std::move(fn);
}

void Host::SetErrorHandler(ErrorHandler fn) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->on_error = std::move(fn);
}

bool Host::StartScreenCapture(std::string* error) {
  if (impl_->capturer) {
    if (error) *error = "screen capture already running";
    return false;
  }
  std::string err;
  auto cap = platform::CreateScreenCapturer(&err);
  if (!cap) {
    if (error) *error = err;
    return false;
  }
  capture::CaptureConfig cc;
  cc.fps = impl_->config.fps;
  cc.width = impl_->config.capture_width;
  cc.height = impl_->config.capture_height;
  cc.show_cursor = impl_->config.capture_cursor;
  Impl* impl = impl_.get();
  if (!cap->Start(cc, [impl](const capture::CapturedFrame& f) {
        // OnFrame reports errors through the error handler itself.
        impl->OnFrame(f, nullptr);
      })) {
    if (error) *error = cap->last_error();
    return false;
  }
  impl_->capturer = std::move(cap);
  if (impl_->config.cursor_channel && !impl_->config.capture_cursor) {
    std::string cerr;
    if (auto src = platform::CreateCursorSource(&cerr)) {
      impl_->cursor_running = true;
      impl_->cursor_thread = std::thread(
          [impl, s = std::move(src)]() mutable { impl->PollCursor(std::move(s)); });
    } else {
      // Not fatal: the screen still streams, just without a cursor.
      impl_->ReportError("cursor channel unavailable: " + cerr);
    }
  }
  return true;
}

void Host::SetCursor(int32_t x, int32_t y, bool visible, uint64_t time_us) {
  impl_->cursor->SetPosition(x, y, visible, time_us ? time_us : capture::MonotonicMicros());
}

void Host::SetCursorImage(const CursorImage& image) { impl_->cursor->SetImage(image); }

void Host::StopScreenCapture() {
  if (impl_->cursor_thread.joinable()) {
    impl_->cursor_running = false;
    impl_->cursor_thread.join();
  }
  if (impl_->capturer) {
    impl_->capturer->Stop();
    impl_->capturer.reset();
  }
}

bool Host::SubmitFrame(const ImageView& bgra, uint64_t capture_time_us, std::span<const Rect> dirty,
                       std::string* error) {
  if (!bgra.data || bgra.width <= 0 || bgra.height <= 0 || bgra.stride < bgra.width * 4) {
    if (error) *error = "invalid image";
    return false;
  }
  capture::CapturedFrame f;
  f.bgra = bgra;
  f.dirty.assign(dirty.begin(), dirty.end());
  f.capture_time_us = capture_time_us;
  return impl_->OnFrame(f, error);
}

void Host::RequestKeyframe() {
  if (auto* s = impl_->session_ptr.load()) s->RequestKeyframe();
  // Before the first frame there is nothing to do: the first frame is always
  // a keyframe.
}

std::string Host::encoder_name() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->encoder_name;
}

HostStats Host::stats() const {
  auto* s = impl_->session_ptr.load();
  HostStats st = s ? s->stats() : HostStats{};
  std::lock_guard<std::mutex> lock(impl_->mu);
  st.frames_dropped_congestion = impl_->frames_dropped_congestion;
  st.frames_resubmitted = impl_->frames_resubmitted;
  st.target_bitrate_bps = impl_->target_bitrate_bps;
  st.bandwidth_estimate_bps = impl_->net.bandwidth_bps;
  st.rtt_us = impl_->net.rtt_us;
  const auto cs = impl_->cursor->stats();
  st.cursor_updates = cs.positions;
  st.cursor_images = cs.images;
  st.cursor_bytes = cs.bytes;
  st.bytes_sent += cs.bytes;
  return st;
}

}  // namespace rdaccel
