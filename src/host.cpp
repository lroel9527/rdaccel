#include "rdacore/host.h"

#include <algorithm>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>

#include "capture/capturer.h"
#if RDACCEL_HAVE_AUDIO
#include "audio/audio_source.h"
#include "audio/sender.h"
#endif
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
  bool CarriesBulk() const override {
    std::shared_lock<std::shared_mutex> l(mu_);
    return inner_->CarriesBulk();
  }
  bool CarriesDatagrams() const override {
    std::shared_lock<std::shared_mutex> l(mu_);
    return inner_->CarriesDatagrams();
  }
  size_t MaxDatagram() const override {
    std::shared_lock<std::shared_mutex> l(mu_);
    return inner_->MaxDatagram();
  }
  // Nothing before the viewer is accepted (as for the channels).
  bool SendDatagram(std::span<const uint8_t> bytes) override {
    std::shared_lock<std::shared_mutex> l(mu_);
    return open_.load() && inner_->SendDatagram(bytes);
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
    const auto full_period = std::chrono::microseconds(1'000'000 / hz);
    // Text mode: a moving pointer at 120 Hz is 16 kbps of positions; at 60
    // it still moves with every frame of a 60 Hz screen.
    const auto text_period = std::chrono::microseconds(1'000'000 / std::min(hz, kTextCursorHz));
    auto next = std::chrono::steady_clock::now();
    capture::CursorSample s;
    CursorImage original;      // as the source reported it
    double sent_factor = 0;    // scale of the image last sent (0 = none sent)
    while (cursor_running) {
      next += text_mode ? text_period : full_period;
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
      AudioPathTick(capture::MonotonicMicros());
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
  // What the transport's datagrams (sound) take of the path, over about the
  // last second, in bits per second (under submit_mu): the picture's rate
  // control leaves it to them.
  uint64_t datagram_rate_time = 0, datagram_rate_bytes = 0;
  int64_t datagram_bps = 0;
  void UpdateDatagramRate(const TransportStats& st, uint64_t now) {
    if (datagram_rate_time == 0 || st.datagram_bytes_sent < datagram_rate_bytes) {
      datagram_rate_time = now;
      datagram_rate_bytes = st.datagram_bytes_sent;
      return;
    }
    if (now - datagram_rate_time < 1'000'000) return;
    datagram_bps = static_cast<int64_t>(static_cast<double>(st.datagram_bytes_sent - datagram_rate_bytes) * 8e6 /
                                        static_cast<double>(now - datagram_rate_time));
    datagram_rate_time = now;
    datagram_rate_bytes = st.datagram_bytes_sent;
  }

  // Text mode (HostConfig::text_mode): as asked for, and as the session and
  // the rate controller were last set up (under submit_mu).
  // The rate control is the same either way: a lower start and floor (for
  // slow paths) left full-screen transitions in blocks on a fast one.
  static constexpr int kTextCursorHz = 60;
  std::atomic<bool> text_mode{false};
  bool text_applied = false;
  int64_t StartBitrate() const { return config.bitrate_bps; }

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
        impl->AudioPeerJoined();
      } else if (st == ConnectionState::kDisconnected) {
        impl->AudioPeerLeft();
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
    l->SetAudio(audio_on);  // offered in the Welcome
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
    datagram_rate_time = 0;
    datagram_bps = 0;
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

  // ---- sound (1.13) ------------------------------------------------------
  // audio_ctl_mu serializes starting and stopping (the source's start and
  // stop run under it, never its callbacks); audio_mu guards the fields,
  // briefly, and is all the callbacks take.
  std::mutex audio_ctl_mu;
  mutable std::mutex audio_mu;
  std::atomic<bool> audio_on{false};
  AudioConfig audio_config;  // under audio_mu
  AudioState audio_state = AudioState::kOff;
  std::string audio_message;
  std::atomic<int32_t> audio_profile{0};
  std::atomic<bool> audio_paused{false};
#if RDACCEL_HAVE_AUDIO
  std::shared_ptr<audio::Sender> sender;       // for the viewer there, when it takes sound
  std::unique_ptr<audio::Source> audio_source;  // under audio_ctl_mu (and audio_mu to read)
  audio::SenderStats audio_done;                // of the senders of earlier viewers
  uint64_t audio_dropped_done = 0;
  // The transport's dropped-datagram count when the current sender began:
  // the transport counts for its whole life, the sound only from then on.
  uint64_t audio_dropped_base = 0;
  // Loss over the last 5 s, for the sender (IdleLoop's thread).
  struct PathSample {
    uint64_t time, sent, lost;
  };
  std::deque<PathSample> audio_path;
  uint64_t audio_path_time = 0;
#endif

  void AudioPeerJoined() {
#if RDACCEL_HAVE_AUDIO
    std::lock_guard<std::mutex> ctl(audio_ctl_mu);
    if (!audio_on) return;
    const auto link = CurrentLink();
    AudioConfig cfg;
    {
      std::lock_guard<std::mutex> l(audio_mu);
      if (sender) return;
      if (!(link->peer_caps() & proto::kCapAudio) || !gate->CarriesDatagrams()) {
        audio_state = AudioState::kWaitingPeer;
        return;
      }
      cfg = audio_config;
    }
    auto s = std::make_shared<audio::Sender>(
        [g = gate.get()](std::span<const uint8_t> b) { return g->SendDatagram(b); }, audio_profile.load());
    s->SetPaused(audio_paused);
    std::unique_ptr<audio::Source> src;
    if (cfg.source == AudioConfig::Source::kTest) src = audio::CreateTestSource();
    if (cfg.source == AudioConfig::Source::kSystem) src = audio::CreateSystemSource();
    TransportStats base;
    const bool have_base = gate->GetStats(&base);
    {
      std::lock_guard<std::mutex> l(audio_mu);
      sender = s;
      audio_dropped_base = have_base ? base.datagrams_dropped : 0;
      audio_message.clear();
      if (cfg.source == AudioConfig::Source::kApp) {
        audio_state = AudioState::kRunning;
        return;
      }
      if (!src) {
        audio_state = AudioState::kFailed;
        audio_message = "this platform has no system sound capture";
        return;
      }
      audio_state = AudioState::kStarting;
    }
    log::Info("sound: the viewer takes it; capture starts");
    audio::SourceConfig sc;
    sc.mute_local = cfg.mute_local;
    sc.exclude_bundle_ids = cfg.exclude_bundle_ids;
    src->Start(
        sc, [s](const float* pcm, size_t frames, uint64_t t) { s->Push(pcm, frames, t); },
        [this](audio::SourceState st, const std::string& msg) { OnSourceState(st, msg); });
    std::lock_guard<std::mutex> l(audio_mu);
    audio_source = std::move(src);
#endif
  }

#if RDACCEL_HAVE_AUDIO
  void OnSourceState(audio::SourceState st, const std::string& msg) {
    std::lock_guard<std::mutex> l(audio_mu);
    AudioState next = AudioState::kStarting;
    switch (st) {
      case audio::SourceState::kStarting: next = AudioState::kStarting; break;
      case audio::SourceState::kRunning: next = AudioState::kRunning; break;
      case audio::SourceState::kNoPermission: next = AudioState::kNoPermission; break;
      case audio::SourceState::kFailed: next = AudioState::kFailed; break;
    }
    if (next != audio_state) {
      // Once per change, not per retry.
      if (next == AudioState::kFailed || next == AudioState::kNoPermission) {
        log::Warn("sound: " + msg);
      } else if (next == AudioState::kRunning) {
        log::Info("sound: capturing");
      }
    }
    audio_state = next;
    audio_message = msg;
  }
#endif

  void AudioPeerLeft() {
#if RDACCEL_HAVE_AUDIO
    std::lock_guard<std::mutex> ctl(audio_ctl_mu);
    std::unique_ptr<audio::Source> src;
    std::shared_ptr<audio::Sender> s;
    {
      std::lock_guard<std::mutex> l(audio_mu);
      src = std::move(audio_source);
      s = std::move(sender);
    }
    if (src) src->Stop();  // no callback after this; the computer's own sound is back
    if (!s) {
      std::lock_guard<std::mutex> l(audio_mu);
      if (audio_on) audio_state = AudioState::kWaitingPeer;
      return;
    }
    TransportStats st;
    const bool have_st = gate->GetStats(&st);
    const auto ss = s->stats();
    std::lock_guard<std::mutex> l(audio_mu);
    audio_done.packets += ss.packets;
    audio_done.bytes += ss.bytes;
    audio_done.frames += ss.frames;
    audio_done.frames_gated += ss.frames_gated;
    audio_done.red_bytes += ss.red_bytes;
    if (have_st) audio_dropped_done += st.datagrams_dropped - std::min(st.datagrams_dropped, audio_dropped_base);
    audio_state = audio_on ? AudioState::kWaitingPeer : AudioState::kOff;
    audio_message.clear();
#endif
  }

  void AudioPathTick(uint64_t now) {
#if RDACCEL_HAVE_AUDIO
    if (now - audio_path_time < 1'000'000) return;
    audio_path_time = now;
    std::shared_ptr<audio::Sender> s;
    {
      std::lock_guard<std::mutex> l(audio_mu);
      s = sender;
    }
    if (!s) {
      audio_path.clear();
      return;
    }
    TransportStats st;
    if (!gate->GetStats(&st)) return;  // no path measured (a pipe)
    const uint64_t lost = st.packets_lost - std::min(st.packets_lost, st.packets_spurious);
    audio_path.push_back({now, st.packets_sent, lost});
    while (audio_path.size() > 1 && now - audio_path.front().time > 5'000'000) audio_path.pop_front();
    const auto& first = audio_path.front();
    const double sent = static_cast<double>(st.packets_sent - std::min(st.packets_sent, first.sent));
    const double loss = sent > 0 ? static_cast<double>(lost - std::min(lost, first.lost)) / sent : 0;
    s->SetPath(loss, st.bandwidth_settled ? st.bandwidth_bps : 0, st.path_queued);
#else
    (void)now;
#endif
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
    // A new session in place of the running one, for the same viewer.
    const bool restart = resized;
    const bool want_text = text_mode;
    if (want_text != text_applied) {
      text_applied = want_text;
      if (session) session->SetTextMode(want_text);
      log::Info(want_text ? "text mode on" : "text mode off");
    }
    if (!session || restart) {
      codec::EncoderConfig cfg{f.bgra.width, f.bgra.height, config.fps, rate ? rate->encoder_bps() : StartBitrate(),
                               config.min_qp};
      std::string err;
      auto enc = platform::CreateVideoEncoder(cfg, &err);
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
      opts.text_mode = text_applied;
      if (config.video_regions) {
        opts.region_encoder_factory = [](const codec::EncoderConfig& c, std::string* e) {
          return platform::CreateVideoEncoder(c, e);
        };
      }
      opts.fps = config.fps;
      opts.bitrate_bps = rate ? rate->encoder_bps() : StartBitrate();
      opts.region_min_qp = config.video_region_min_qp;
      opts.region_bitrate_bps = config.video_region_bitrate_bps;
      opts.own_receiver = false;  // the link receives and passes on
      // After a resolution change the new Hello travels with the
      // frames, so the viewer switches exactly between the old and new
      // pictures.
      opts.hello_on_video = restart;
      HostStats carried;
      if (restart) {
        carried = session->stats();
        opts.first_frame_id = session->next_frame_id();
        std::lock_guard<std::mutex> l(mu);
        if (resized) ++resolution_changes;
      }
      auto next = std::make_unique<session::HostSession>(gate.get(), std::move(enc), width,
                                                         height, opts);
      if (restart) {
        next->AdoptStats(carried);
      } else if (carry_next) {  // the next viewer: the counts go on
        next->AdoptStats(*carry_next);
        carry_next.reset();
      }
      if (rate && restart) next->SetBitrate(rate->encoder_bps());
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
      // The sound's share of the path is not the picture's to take.
      UpdateDatagramRate(st, capture::MonotonicMicros());
      TransportStats for_picture = st;
      if (datagram_bps > 0 && st.bandwidth_bps > 0) {
        for_picture.bandwidth_bps =
            std::max(st.bandwidth_bps - datagram_bps, std::min(config.min_bitrate_bps, st.bandwidth_bps));
      }
      // What shows whether the target is used: the encoders' output, or in
      // text mode, where they hardly send, everything the session sends
      // (judged by video alone the target only ever came down: 46 kbps on a
      // 100 kbps path).
      const auto d = rate->Update(capture::MonotonicMicros(), for_picture, text_applied ? ss.bytes_sent : ss.video_bytes,
                                  ss.bytes_sent);
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
    // Whether this viewer decodes context tiles (0 before it has said hello:
    // nothing is sent to it then anyway).
    if (text_applied) {
      const uint64_t caps = CurrentLink()->peer_caps();
      session->SetContextTiles((caps & proto::kCapContextTiles) != 0, (caps & proto::kCapContextTiles64) != 0);
    }
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
  impl_->text_mode = impl_->config.text_mode;
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
  StopAudio();  // the computer's own sound comes back first
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

bool Host::SendBulkMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error) {
  return impl_->CurrentLink()->SendBulkMessage(type, payload, error);
}

int64_t Host::BulkQueuedBytes() const { return impl_->CurrentLink()->BulkQueuedBytes(); }

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

void Host::SetTextMode(bool on) { impl_->text_mode = on; }
bool Host::text_mode() const { return impl_->text_mode; }

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
  st.fec_parity_sent = impl_->net.fec_parity_sent;
  st.fec_recovered = impl_->net.fec_recovered;
  const auto cs = impl_->cursor->stats();
  st.cursor_updates = cs.positions;
  st.cursor_images = cs.images;
  st.cursor_bytes = cs.bytes;
  st.bytes_sent += cs.bytes;
#if RDACCEL_HAVE_AUDIO
  {
    std::shared_ptr<audio::Sender> s;
    audio::SenderStats a;
    uint64_t dropped = 0, base = 0;
    {
      std::lock_guard<std::mutex> l(impl_->audio_mu);
      s = impl_->sender;
      a = impl_->audio_done;
      dropped = impl_->audio_dropped_done;
      base = impl_->audio_dropped_base;
    }
    if (s) {
      const auto c = s->stats();
      a.packets += c.packets;
      a.bytes += c.bytes;
      a.frames += c.frames;
      a.frames_gated += c.frames_gated;
      a.red_bytes += c.red_bytes;
      TransportStats ts;
      if (impl_->gate->GetStats(&ts)) dropped += ts.datagrams_dropped - std::min(ts.datagrams_dropped, base);
    }
    st.audio_packets = a.packets;
    st.audio_bytes = a.bytes;
    st.audio_frames = a.frames;
    st.audio_frames_gated = a.frames_gated;
    st.audio_red_bytes = a.red_bytes;
    st.audio_dropped_stale = dropped;
    st.bytes_sent += a.bytes;
  }
#endif
  return st;
}

bool Host::StartAudio(const AudioConfig& config, std::string* error) {
#if RDACCEL_HAVE_AUDIO
  if (config.source == AudioConfig::Source::kSystem && !audio::CreateSystemSource()) {
    if (error) *error = "this platform has no system sound capture";
    return false;
  }
  StopAudio();
  {
    std::lock_guard<std::mutex> ctl(impl_->audio_ctl_mu);
    std::lock_guard<std::mutex> l(impl_->audio_mu);
    impl_->audio_config = config;
    impl_->audio_profile = config.profile == 1 ? 1 : 0;
    impl_->audio_state = AudioState::kWaitingPeer;
    impl_->audio_message.clear();
    impl_->audio_on = true;
  }
  const auto link = impl_->CurrentLink();
  link->SetAudio(true);
  // A viewer may be there already.
  if (link->state() == ConnectionState::kConnected) impl_->AudioPeerJoined();
  return true;
#else
  (void)config;
  if (error) *error = "this library was built without sound (RDACCEL_AUDIO=OFF)";
  return false;
#endif
}

void Host::StopAudio() {
  if (!impl_->audio_on.exchange(false)) return;
  impl_->CurrentLink()->SetAudio(false);
  impl_->AudioPeerLeft();
  std::lock_guard<std::mutex> l(impl_->audio_mu);
  impl_->audio_state = AudioState::kOff;
  impl_->audio_message.clear();
}

bool Host::SubmitAudio(const float* interleaved, size_t frames, uint64_t capture_time_us) {
#if RDACCEL_HAVE_AUDIO
  std::shared_ptr<audio::Sender> s;
  {
    std::lock_guard<std::mutex> l(impl_->audio_mu);
    if (!impl_->audio_on || impl_->audio_config.source != AudioConfig::Source::kApp) return false;
    s = impl_->sender;
  }
  if (!s) return false;
  s->Push(interleaved, frames, capture_time_us);
  return true;
#else
  (void)interleaved;
  (void)frames;
  (void)capture_time_us;
  return false;
#endif
}

void Host::SetAudioProfile(int32_t profile) {
  impl_->audio_profile = profile == 1 ? 1 : 0;
#if RDACCEL_HAVE_AUDIO
  std::lock_guard<std::mutex> l(impl_->audio_mu);
  if (impl_->sender) impl_->sender->SetProfile(impl_->audio_profile);
#endif
}

void Host::SetAudioPaused(bool paused) {
  impl_->audio_paused = paused;
#if RDACCEL_HAVE_AUDIO
  std::lock_guard<std::mutex> l(impl_->audio_mu);
  if (impl_->sender) impl_->sender->SetPaused(paused);
#endif
}

AudioStatus Host::audio_status() const {
  AudioStatus a;
  std::lock_guard<std::mutex> l(impl_->audio_mu);
  a.state = impl_->audio_state;
  a.message = impl_->audio_message;
  a.profile = impl_->audio_profile;
#if RDACCEL_HAVE_AUDIO
  if (impl_->sender) {
    const auto ss = impl_->sender->status();
    a.silent = ss.silent;
    a.bitrate_bps = ss.bitrate_bps;
    a.frame_ms = ss.frame_ms;
    a.channels = ss.channels;
    if (a.state == AudioState::kRunning && impl_->audio_paused) a.state = AudioState::kPaused;
  }
  if (impl_->audio_source) a.muted_local = impl_->audio_source->muting_local();
#endif
  return a;
}

}  // namespace rdacore
