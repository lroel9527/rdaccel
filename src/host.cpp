#include "rdacore/host.h"

#include <algorithm>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>

#include "capture/capturer.h"
#include "log/log.h"
#include "platform.h"
#include "session/cursor_channel.h"
#include "session/host_session.h"
#include "session/link.h"
#include "transport/secure_transport.h"
#include "session/rate_control.h"

namespace rdacore {

namespace {

// What the session and the cursor channel send on. With an auth hook it
// passes nothing until the link has accepted the viewer, so a viewer that
// is refused never sees the screen size, a picture or the pointer. Until
// then PeerKnown() is false, which makes the host hold frames instead of
// encoding them; on connect it sends the Hello, the cursor and a keyframe
// afresh. Without a hook every viewer that completes the handshake is let
// in, and the gate stays open from the start (recordings and other
// transports without a viewer at the far end need that).
class ViewerGate final : public Transport {
 public:
  ViewerGate(Transport* inner, bool open) : inner_(inner), open_(open) {}
  void Open() { open_ = true; }
  bool is_open() const { return open_.load(); }
  // The next viewer's transport (the old one is still alive during the call).
  void Retarget(Transport* inner, bool open) {
    std::unique_lock<std::shared_mutex> l(mu_);
    inner_ = inner;
    open_ = open;
  }
  bool Send(Channel ch, std::span<const uint8_t> bytes) override {
    std::shared_lock<std::shared_mutex> l(mu_);
    return open_.load() ? inner_->Send(ch, bytes) : true;
  }
  void SetReceiver(ReceiveFn) override {}  // the link receives
  void Close() override {}
  bool PeerKnown() const override {
    std::shared_lock<std::shared_mutex> l(mu_);
    return open_.load() && inner_->PeerKnown();
  }
  bool GetStats(TransportStats* out) const override {
    std::shared_lock<std::shared_mutex> l(mu_);
    return inner_->GetStats(out);
  }

 private:
  mutable std::shared_mutex mu_;
  Transport* inner_;
  std::atomic<bool> open_;
};

}  // namespace

struct Host::Impl {
  HostConfig config;

  // Created on the first frame, when the resolution is known, and replaced
  // when it changes. Replaced under both submit_mu and session_mu: the frame
  // path (under submit_mu) uses it directly, other threads via WithSession.
  std::unique_ptr<session::HostSession> session;
  std::mutex session_mu;
  template <class F>
  void WithSession(F&& f) {
    std::lock_guard<std::mutex> l(session_mu);
    if (session) f(*session);
  }
  std::unique_ptr<capture::ICapturer> capturer;
  std::atomic<bool> screen_capture{false};  // capturer running (read by the idle thread)
  // Handshake, keepalive, messages, for the current viewer. Calls into the
  // session; destroyed after the transport (whose receive thread calls it).
  // Replaced for each viewer (see ServeNext); read it through CurrentLink().
  std::shared_ptr<session::Link> link;
  mutable std::mutex link_mu;
  std::shared_ptr<session::Link> CurrentLink() const {
    std::lock_guard<std::mutex> l(link_mu);
    return link;
  }
  // Declared after `session` and `link`, so it is destroyed first: its
  // destructor stops the receive thread that calls into them.
  std::unique_ptr<Transport> transport;
  // On `transport`; what the session and the cursor send on.
  std::unique_ptr<ViewerGate> gate;
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
  // A frame was skipped since the last one that reached the session: its
  // changes are not in the next frame's dirty rects (those are relative to
  // the source's previous frame), so the next frame is compared in full.
  bool full_diff_next = false;  // under submit_mu
  uint64_t frames_resubmitted = 0;  // under mu
  uint64_t resolution_changes = 0;  // under mu

  void IdleLoop() {
    std::unique_lock<std::mutex> lock(idle_mu);
    while (!idle_cv.wait_for(lock, std::chrono::milliseconds(25), [&] { return idle_stop; })) {
      if (failed) continue;
      const uint64_t now = capture::MonotonicMicros();
      if (now - last_frame_us.load() < kIdleResubmitUs) continue;
      // Under submit_mu throughout, so no newer frame (or new session) can
      // go in between.
      std::lock_guard<std::mutex> l(submit_mu);
      session::HostSession* s = session.get();
      if (!s) continue;
      std::vector<uint8_t> px;
      if (!skipped_px.empty()) {
        px = std::move(skipped_px);
        skipped_px.clear();
      } else if (s->keyframe_pending() || (screen_capture && s->work_pending())) {
        // Screen capture (ScreenCaptureKit, PipeWire) delivers no frames
        // while nothing changes, so what the last change left owed would
        // wait for the next one. An application feeding frames itself
        // decides when to send them.
        px = s->last_frame();
      } else {
        continue;
      }
      if (CurrentLink()->disconnected()) continue;  // nobody to send to
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
  StateHandler on_state;
  MessageHandler on_message;
  uint64_t keyframe_requests = 0;

  // From the link (transport thread): input and keyframe requests.
  void OnPeerMessage(const proto::Message& m) {
    if (auto* in = std::get_if<proto::InputEvent>(&m)) {
      InputHandler fn;
      {
        std::lock_guard<std::mutex> l(mu);
        fn = on_input;
      }
      if (fn) fn(*in);
    } else if (std::holds_alternative<proto::KeyframeRequest>(m)) {
      {
        std::lock_guard<std::mutex> l(mu);
        ++keyframe_requests;
      }
      WithSession([](session::HostSession& s) { s.RequestKeyframe(); });
    }
  }

  void ReportError(const std::string& msg) {
    ErrorHandler fn;
    {
      std::lock_guard<std::mutex> lock(mu);
      fn = on_error;
    }
    if (fn) fn(msg);
  }

  void NotifyState(ConnectionState st, ConnectionError e) {
    StateHandler fn;
    {
      std::lock_guard<std::mutex> l(mu);
      fn = on_state;
    }
    if (fn) fn(st, e);
  }

  session::Link::Handlers LinkHandlers() {
    Impl* impl = this;
    session::Link::Handlers h;
    h.on_state = [impl](ConnectionState st, ConnectionError e) {
      // A viewer that arrives mid-stream needs the Hello and a full picture.
      if (st == ConnectionState::kConnected) {
        const bool was_closed = !impl->gate->is_open();
        if (was_closed) impl->gate->Open();
        if (impl->resync_cursor.exchange(false) || was_closed) impl->cursor->Resync();
        impl->WithSession([](session::HostSession& s) {
          s.SendHello();
          s.RequestKeyframe();
        });
      }
      impl->NotifyState(st, e);
      // After the application has heard of it: then wait for the next one.
      if (st == ConnectionState::kDisconnected && impl->config.keep_serving) {
        {
          std::lock_guard<std::mutex> l(impl->serve_mu);
          impl->serve_due = true;
        }
        impl->serve_cv.notify_all();
      }
    };
    h.on_message = [impl](uint32_t type, std::span<const uint8_t> payload) {
      MessageHandler fn;
      {
        std::lock_guard<std::mutex> l(impl->mu);
        fn = impl->on_message;
      }
      if (fn) fn(type, payload);
    };
    h.on_other = [impl](const proto::Message& m) { impl->OnPeerMessage(m); };
    h.on_auth = config.auth;
    return h;
  }

  // Makes `t` the current transport, with a new link on it, and starts it.
  // Returns the transport and link it replaces, for the caller to destroy
  // (transport first: its receive thread calls the link). The gate must
  // already point at `t`.
  std::pair<std::unique_ptr<Transport>, std::shared_ptr<session::Link>> Attach(
      std::unique_ptr<Transport> t, bool announce) {
    auto* secure = dynamic_cast<transport::SecureTransport*>(t.get());
    session::LinkOptions lo;
    lo.peer_timeout_ms = config.peer_timeout_ms;
    auto l = std::make_shared<session::Link>(session::Link::Role::kHost, t.get(), lo, LinkHandlers());
    if (secure) {
      secure->SetErrorHandler([lp = l.get()](ConnectionError e, const std::string& msg) {
        log::Warn("host: " + msg);
        lp->Abort(e);
      });
    }
    std::unique_ptr<Transport> old_t = std::move(transport);
    transport = std::move(t);
    std::shared_ptr<session::Link> old_l;
    {
      std::lock_guard<std::mutex> g(link_mu);
      old_l = std::move(link);
      link = l;
    }
    // Before anything from the new viewer can arrive (the receiver is not
    // set yet), so the application sees the states in order.
    if (announce) NotifyState(ConnectionState::kConnecting, ConnectionError::kNone);
    transport->SetReceiver(
        [lp = l.get()](Channel ch, std::span<const uint8_t> bytes) { lp->OnReceive(ch, bytes); });
    l->Start();
    return {std::move(old_t), std::move(old_l)};
  }

  // One viewer after another (config.keep_serving): when the viewer has gone
  // (left, timed out, refused, wrong key), the transport is asked for the
  // next peer on the same port. Capture and the encoder's input keep going;
  // the next viewer gets a session of its own, from a keyframe.
  std::thread serve_thread;
  std::mutex serve_mu;
  std::condition_variable serve_cv;
  bool serve_due = false, serve_stop = false;
  std::atomic<bool> resync_cursor{false};
  std::optional<HostStats> carry_next;  // under submit_mu

  void ServeLoop() {
    std::unique_lock<std::mutex> lock(serve_mu);
    for (;;) {
      serve_cv.wait(lock, [&] { return serve_due || serve_stop; });
      if (serve_stop) return;
      serve_due = false;
      lock.unlock();
      ServeNext();
      lock.lock();
    }
  }

  void ServeNext() {
    std::lock_guard<std::mutex> lock(submit_mu);  // no frames meanwhile
    auto old_link = CurrentLink();
    if (!old_link->disconnected()) return;
    old_link->Shutdown();  // its timer thread is gone after this
    std::string err;
    // Closes the old transport (its receive thread is gone after this).
    auto next = transport->NextPeer(&err);
    if (!next) {
      log::Info("host: not waiting for another viewer (" + err + ")");
      return;
    }
    // The next viewer starts from nothing: a new session (from the newest
    // frame, kept here), a new bitrate estimate.
    std::vector<uint8_t> px;
    int32_t w = 0, h = 0;
    std::unique_ptr<session::HostSession> old_session;
    {
      std::lock_guard<std::mutex> s(session_mu);
      if (session) {
        carry_next = session->stats();
        px = skipped_px.empty() ? session->last_frame() : std::move(skipped_px);
        w = session->width();
        h = session->height();
      }
      old_session = std::move(session);
    }
    old_session.reset();  // before the gate points at the next viewer
    skipped_px.clear();
    rate.reset();
    {
      std::lock_guard<std::mutex> m(mu);
      net = {};
      target_bitrate_bps = 0;
    }
    resync_cursor = true;
    gate->Retarget(next.get(), /*open=*/!config.auth);
    auto [old_t, old_l] = Attach(std::move(next), /*announce=*/true);
    old_t.reset();
    old_l.reset();
    if (!px.empty() && !failed) {
      capture::CapturedFrame f;
      f.bgra = {px.data(), w, h, w * 4};
      f.capture_time_us = capture::MonotonicMicros();
      OnFrameLocked(f, nullptr);
    }
  }

  bool OnFrame(const capture::CapturedFrame& f, std::string* error) {
    std::lock_guard<std::mutex> lock(submit_mu);
    if (auto l = CurrentLink(); l->disconnected()) {
      if (error) *error = std::string("disconnected: ") + session::LinkErrorText(l->error());
      return false;
    }
    return OnFrameLocked(f, error);
  }

  void KeepSkipped(const capture::CapturedFrame& f) {
    full_diff_next = true;
    skipped_px.resize(static_cast<size_t>(width) * height * 4);
    for (int32_t y = 0; y < height; ++y) {
      std::memcpy(skipped_px.data() + static_cast<size_t>(y) * width * 4,
                  f.bgra.data + static_cast<size_t>(y) * f.bgra.stride, static_cast<size_t>(width) * 4);
    }
  }

  bool OnFrameLocked(const capture::CapturedFrame& f, std::string* error) {
    last_frame_us = capture::MonotonicMicros();
    if (failed) {
      if (error) *error = "host stopped after an earlier encoder error";
      return false;
    }
    const bool resized = session && (f.bgra.width != width || f.bgra.height != height);
    if (!session || resized) {
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
      if (resized) {
        log::Info("resolution " + std::to_string(width) + "x" + std::to_string(height) + " -> " +
                  std::to_string(f.bgra.width) + "x" + std::to_string(f.bgra.height));
      }
      width = f.bgra.width;
      height = f.bgra.height;
      frame_w = width;
      frame_h = height;
      skipped_px.clear();
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
      opts.own_receiver = false;  // the link receives and passes on
      // After a resolution change the new Hello travels with the frames, so
      // the viewer switches sizes exactly between the old and new pictures.
      opts.hello_on_video = resized;
      HostStats carried;
      if (resized) {
        carried = session->stats();
        opts.first_frame_id = session->next_frame_id();
        std::lock_guard<std::mutex> l(mu);
        ++resolution_changes;
      }
      auto next = std::make_unique<session::HostSession>(gate.get(), std::move(enc), width,
                                                         height, opts);
      if (resized) {
        next->AdoptStats(carried);
      } else if (carry_next) {  // the next viewer: the counts go on
        next->AdoptStats(*carry_next);
        carry_next.reset();
      }
      if (rate && resized) next->SetBitrate(rate->target_bps());
      std::unique_ptr<session::HostSession> old;
      {
        std::lock_guard<std::mutex> l(session_mu);
        old = std::move(session);
        session = std::move(next);
      }
    }
    // A listening transport without a peer yet: keep the newest frame
    // (re-submitted once the viewer is there) instead of queueing pictures
    // nobody receives.
    if (!gate->PeerKnown()) {
      KeepSkipped(f);
      return true;
    }
    // Adaptive bitrate: follow what the transport measures.
    TransportStats st;
    if (config.adaptive_bitrate && transport->GetStats(&st)) {
      if (!rate) {
        rate = std::make_unique<session::RateController>(
            config.bitrate_bps, std::min(config.min_bitrate_bps, config.bitrate_bps), config.max_queue_ms);
      }
      const auto ss = session->stats();
      const auto d = rate->Update(capture::MonotonicMicros(), st, ss.video_bytes, ss.bytes_sent);
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
        KeepSkipped(f);
        return true;
      }
      skipped_px.clear();
    }
    std::string err;
    capture::CapturedFrame whole;
    const capture::CapturedFrame* in = &f;
    if (full_diff_next && !f.dirty.empty()) {
      whole = f;
      whole.dirty.clear();  // empty = compare every tile
      in = &whole;
    }
    full_diff_next = false;
    if (!session->OnFrame(*in, &err)) {
      if (error) *error = err;
      // One picture lost, the next one is a keyframe: not something the
      // error handler reports (it reports only what stops the host). Logged
      // the first time and then once per second's worth of frames; counted
      // in encode_errors.
      if (encode_errors_reported++ % static_cast<uint64_t>(std::max(1, config.fps)) == 0) {
        log::Warn("encode failed: " + err);
      }
      return false;
    }
    return true;
  }
};

Host::Host(std::unique_ptr<Transport> transport, HostConfig config)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->on_state = std::move(impl_->config.on_state);
  impl_->on_message = std::move(impl_->config.on_message);
  if (!impl_->config.encryption_key.empty()) {
    if (impl_->config.encryption_key.size() != transport::kSecureKeySize) {
      throw std::invalid_argument("encryption_key must be 32 bytes");
    }
    auto s = std::make_unique<transport::SecureTransport>(
        std::move(transport), /*is_host=*/true,
        std::span<const uint8_t, transport::kSecureKeySize>(impl_->config.encryption_key.data(),
                                                            transport::kSecureKeySize));
    transport = std::move(s);
  }
  impl_->gate = std::make_unique<ViewerGate>(transport.get(), /*open=*/!impl_->config.auth);
  impl_->cursor = std::make_unique<session::CursorSender>(impl_->gate.get());
  impl_->Attach(std::move(transport), /*announce=*/false);
  if (impl_->config.keep_serving) {
    impl_->serve_thread = std::thread([impl = impl_.get()] { impl->ServeLoop(); });
  }
  impl_->idle_thread = std::thread([impl = impl_.get()] { impl->IdleLoop(); });
}

Host::~Host() {
  if (impl_->serve_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lock(impl_->serve_mu);
      impl_->serve_stop = true;
    }
    impl_->serve_cv.notify_all();
    impl_->serve_thread.join();
  }
  StopScreenCapture();
  {
    std::lock_guard<std::mutex> lock(impl_->idle_mu);
    impl_->idle_stop = true;
  }
  impl_->idle_cv.notify_all();
  impl_->idle_thread.join();
  impl_->CurrentLink()->Shutdown();  // Goodbye while the transport still works
  impl_->cursor.reset();
  // Stop the receive thread before the link and session go away.
  impl_->transport.reset();
}

void Host::SetStateHandler(StateHandler fn) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->on_state = std::move(fn);
}

ConnectionState Host::state() const { return impl_->CurrentLink()->state(); }

void Host::SetMessageHandler(MessageHandler fn) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->on_message = std::move(fn);
}

bool Host::SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error) {
  return impl_->CurrentLink()->SendMessage(type, payload, error);
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
  cc.display_index = impl_->config.display_index;
  cc.fps = impl_->config.fps;
  cc.width = impl_->config.capture_width;
  cc.height = impl_->config.capture_height;
  cc.show_cursor = impl_->config.capture_cursor;
  cc.include_own_windows = impl_->config.capture_own_windows;
  Impl* impl = impl_.get();
  cap->SetStoppedHandler([impl](const std::string& why) { impl->ReportError("capture stopped: " + why); });
  if (!cap->Start(cc, [impl](const capture::CapturedFrame& f) {
        // OnFrame reports errors through the error handler itself.
        impl->OnFrame(f, nullptr);
      })) {
    if (error) *error = cap->last_error();
    return false;
  }
  impl_->capturer = std::move(cap);
  impl_->screen_capture = true;
  if (impl_->config.cursor_channel && !impl_->config.capture_cursor) {
    std::string cerr;
    if (auto src = platform::CreateCursorSource(impl_->config.display_index, &cerr)) {
      impl_->cursor_running = true;
      impl_->cursor_thread = std::thread(
          [impl, s = std::move(src)]() mutable { impl->PollCursor(std::move(s)); });
    } else if (!cerr.empty()) {
      // Not fatal: the screen still streams, just without a cursor. (An
      // empty error: the capture shows the cursor in the picture itself.)
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
  impl_->screen_capture = false;
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
  impl_->WithSession([](session::HostSession& s) { s.RequestKeyframe(); });
  // Before the first frame there is nothing to do: the first frame is always
  // a keyframe.
}

std::string Host::encoder_name() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->encoder_name;
}

HostStats Host::stats() const {
  HostStats st;
  impl_->WithSession([&](session::HostSession& s) { st = s.stats(); });
  std::lock_guard<std::mutex> lock(impl_->mu);
  st.frames_dropped_congestion = impl_->frames_dropped_congestion;
  st.frames_resubmitted = impl_->frames_resubmitted;
  st.resolution_changes = impl_->resolution_changes;
  st.keyframe_requests = impl_->keyframe_requests;
  st.target_bitrate_bps = impl_->target_bitrate_bps;
  st.bandwidth_estimate_bps = impl_->net.bandwidth_bps;
  st.rtt_us = impl_->net.rtt_us;
  st.packets_sent = impl_->net.packets_sent;
  st.packets_lost = impl_->net.packets_lost;
  st.packets_spurious = impl_->net.packets_spurious;
  const auto cs = impl_->cursor->stats();
  st.cursor_updates = cs.positions;
  st.cursor_images = cs.images;
  st.cursor_bytes = cs.bytes;
  st.bytes_sent += cs.bytes;
  return st;
}

}  // namespace rdacore
