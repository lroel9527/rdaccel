// rdaccel C++ API: a header-only wrapper over the C ABI in rdaccel.h.
//
//   rdaccel::Host       capture or receive screen frames, encode, send
//   rdaccel::Viewer     receive, decode, hand pictures to the application
//   rdaccel::Transport  in-process pipe, TCP, UDP, or your own subclass
//
// Everything here is inline and calls only rda_* functions, so the binary
// interface between your program and the library is the C one: you can build
// with another compiler or standard library than the library was built with.
// Requires C++20. See docs/integration.md.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rdaccel/rdaccel.h"

namespace rdaccel {

// ---- basics -----------------------------------------------------------------

inline const char* Version() { return rda_version_string(); }
inline const char* PlatformName() { return rda_platform_name(); }
inline uint64_t NowMicros() { return rda_now_us(); }

// The text of the last failing rda_* call on this thread.
inline std::string LastError() { return rda_last_error(); }

enum class LogLevel { kError = RDA_LOG_ERROR, kWarn = RDA_LOG_WARN, kInfo = RDA_LOG_INFO, kDebug = RDA_LOG_DEBUG };
using LogFn = std::function<void(LogLevel, const char* message)>;

// Process-wide log sink (see rda_set_log). An empty function removes it.
inline void SetLogHandler(LogFn fn, LogLevel max_level = LogLevel::kInfo) {
  if (!fn) {
    rda_set_log(nullptr, nullptr, RDA_LOG_ERROR);
    return;
  }
  // Kept alive for the rest of the process: a message may be in flight.
  static std::mutex mu;
  static std::vector<std::unique_ptr<LogFn>> holders;
  LogFn* holder;
  {
    std::lock_guard<std::mutex> lock(mu);
    holders.push_back(std::make_unique<LogFn>(std::move(fn)));
    holder = holders.back().get();
  }
  rda_set_log([](void* u, rda_log_level l, const char* m) { (*static_cast<LogFn*>(u))(static_cast<LogLevel>(l), m); },
              holder, static_cast<rda_log_level>(max_level));
}

// A pixel rectangle (origin top-left, physical pixels).
struct Rect {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;

  bool empty() const { return w <= 0 || h <= 0; }
  int32_t right() const { return x + w; }
  int32_t bottom() const { return y + h; }
  bool intersects(const Rect& o) const {
    return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
  }
  friend bool operator==(const Rect&, const Rect&) = default;
};

inline constexpr int32_t kTileSize = RDA_TILE_SIZE;

// A read-only BGRA image.
struct ImageView {
  const uint8_t* data = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;  // bytes per row
};

// ---- cursor -----------------------------------------------------------------

struct CursorImage {
  int32_t width = 0;  // at most 256 x 256
  int32_t height = 0;
  int32_t hotspot_x = 0;
  int32_t hotspot_y = 0;
  std::vector<uint8_t> bgra;  // width * height * 4, premultiplied alpha
  friend bool operator==(const CursorImage&, const CursorImage&) = default;
};

struct CursorState {
  int32_t x = 0;  // host pixels, where the hotspot goes
  int32_t y = 0;
  bool visible = true;
  uint64_t time_us = 0;
  uint64_t image_id = 0;  // 0 = no image yet (draw a default arrow)
  bool image_changed = false;
  const CursorImage* image = nullptr;  // valid during the call
};

inline void DrawCursor(const CursorImage& c, int32_t x, int32_t y, uint8_t* bgra, int32_t width,
                       int32_t height, int32_t stride) {
  if (c.bgra.size() != static_cast<size_t>(c.width) * c.height * 4) return;
  const rda_cursor_image ci{c.width, c.height, c.hotspot_x, c.hotspot_y, c.bgra.data()};
  rda_draw_cursor(&ci, x, y, bgra, width, height, stride);
}

// ---- input ------------------------------------------------------------------

enum class InputKind : uint8_t {
  kMouseMove = RDA_INPUT_MOUSE_MOVE,
  kMouseDown = RDA_INPUT_MOUSE_DOWN,
  kMouseUp = RDA_INPUT_MOUSE_UP,
  kScroll = RDA_INPUT_SCROLL,
  kKeyDown = RDA_INPUT_KEY_DOWN,
  kKeyUp = RDA_INPUT_KEY_UP,
};

struct InputEvent {
  InputKind kind = InputKind::kMouseMove;
  int32_t x = 0;
  int32_t y = 0;
  int32_t delta_x = 0;
  int32_t delta_y = 0;
  uint32_t code = 0;
  uint32_t modifiers = 0;
  friend bool operator==(const InputEvent&, const InputEvent&) = default;
};

// Mouse buttons (InputEvent::code of mouse events) and modifier bits.
inline constexpr uint32_t kButtonLeft = RDA_BUTTON_LEFT;
inline constexpr uint32_t kButtonRight = RDA_BUTTON_RIGHT;
inline constexpr uint32_t kButtonMiddle = RDA_BUTTON_MIDDLE;
inline constexpr uint32_t kButtonBack = RDA_BUTTON_BACK;
inline constexpr uint32_t kButtonForward = RDA_BUTTON_FORWARD;
inline constexpr uint32_t kModShift = RDA_MOD_SHIFT;
inline constexpr uint32_t kModCtrl = RDA_MOD_CTRL;
inline constexpr uint32_t kModAlt = RDA_MOD_ALT;
inline constexpr uint32_t kModMeta = RDA_MOD_META;
inline constexpr uint32_t kModCaps = RDA_MOD_CAPS;

// Key events carry USB HID usages (keyboard page 0x07). From native codes:
// 0 = unknown key. To native codes: kNoKeyCode = the OS has no such key.
inline constexpr uint32_t kNoKeyCode = RDA_KEYCODE_NONE;
inline uint32_t KeyCodeFromMac(uint32_t mac_keycode) { return rda_keycode_from_mac(mac_keycode); }
inline uint32_t KeyCodeToMac(uint32_t hid_usage) { return rda_keycode_to_mac(hid_usage); }
inline uint32_t KeyCodeFromEvdev(uint32_t evdev_code) { return rda_keycode_from_evdev(evdev_code); }
inline uint32_t KeyCodeToEvdev(uint32_t hid_usage) { return rda_keycode_to_evdev(hid_usage); }

// ---- connection -------------------------------------------------------------

enum class ConnectionState {
  kConnecting = RDA_STATE_CONNECTING,
  kConnected = RDA_STATE_CONNECTED,
  kDisconnected = RDA_STATE_DISCONNECTED,  // final
};

// Application message types: library types below kFirstUserMessage.
inline constexpr uint32_t kClipboardTextMessage = RDA_MSG_CLIPBOARD_TEXT;  // UTF-8
inline constexpr uint32_t kFirstUserMessage = RDA_MSG_USER_FIRST;
inline constexpr size_t kMaxMessageSize = RDA_MSG_MAX_SIZE;

// `reason`: RDA_OK, or for kDisconnected RDA_ERR_CLOSED, RDA_ERR_TIMEOUT,
// RDA_ERR_PROTOCOL or RDA_ERR_AUTH. Use rda_status_str() for text.
using StateHandler = std::function<void(ConnectionState state, rda_status reason)>;
using MessageHandler = std::function<void(uint32_t type, std::span<const uint8_t> payload)>;

namespace detail {

inline rda_input_event ToC(const InputEvent& e) {
  rda_input_event c;
  rda_input_event_default(&c);
  c.kind = static_cast<rda_input_kind>(e.kind);
  c.x = e.x;
  c.y = e.y;
  c.delta_x = e.delta_x;
  c.delta_y = e.delta_y;
  c.code = e.code;
  c.modifiers = e.modifiers;
  return c;
}

inline InputEvent FromC(const rda_input_event& c) {
  return {static_cast<InputKind>(c.kind), c.x, c.y, c.delta_x, c.delta_y, c.code, c.modifiers};
}

inline bool SetError(std::string* error) {
  if (error) *error = rda_last_error();
  return false;
}

}  // namespace detail

// ---- transport --------------------------------------------------------------

enum class Channel : uint8_t {
  kControl = RDA_CHANNEL_CONTROL,
  kInput = RDA_CHANNEL_INPUT,
  kSurface = RDA_CHANNEL_SURFACE,
  kVideo = RDA_CHANNEL_VIDEO,
  kCursor = RDA_CHANNEL_CURSOR,
  kMessage = RDA_CHANNEL_MESSAGE,
};

struct TransportStats {
  int64_t bandwidth_bps = 0;
  uint32_t rtt_us = 0;
  uint64_t queued_bytes = 0;
  uint64_t bytes_in_flight = 0;
  uint64_t packets_sent = 0;
  uint64_t packets_lost = 0;
};

class Transport;
namespace detail {
rda_transport* IntoHandle(std::unique_ptr<Transport> t);
}

// Subclass this to plug in your own transport; the requirements are those of
// rda_transport_vtable in rdaccel.h. The library's own transports (from
// ListenUdp() etc.) are also Transports.
class Transport {
 public:
  using ReceiveFn = std::function<void(Channel, std::span<const uint8_t>)>;

  virtual ~Transport() = default;
  virtual bool Send(Channel ch, std::span<const uint8_t> bytes) = 0;
  virtual void SetReceiver(ReceiveFn fn) = 0;
  virtual void Close() = 0;
  virtual bool GetStats(TransportStats* out) const {
    (void)out;
    return false;
  }

 private:
  friend rda_transport* detail::IntoHandle(std::unique_ptr<Transport> t);
  // Library transports hand over their C handle; subclasses are wrapped.
  virtual rda_transport* ReleaseNativeHandle() { return nullptr; }
};

namespace detail {

// One of the library's transports, owned through its C handle.
class NativeTransport final : public Transport {
 public:
  explicit NativeTransport(rda_transport* t) : t_(t) {}
  ~NativeTransport() override { rda_transport_destroy(t_); }

  bool Send(Channel ch, std::span<const uint8_t> bytes) override {
    return t_ && rda_transport_send(t_, static_cast<rda_channel>(ch), bytes.data(), bytes.size()) == RDA_OK;
  }
  void SetReceiver(ReceiveFn fn) override {
    if (!t_) return;
    if (!fn) {
      rda_transport_set_receiver(t_, nullptr, nullptr);
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    receivers_.push_back(std::make_unique<ReceiveFn>(std::move(fn)));
    rda_transport_set_receiver(
        t_,
        [](void* ctx, rda_channel ch, const uint8_t* b, size_t n) {
          (*static_cast<ReceiveFn*>(ctx))(static_cast<Channel>(ch), std::span<const uint8_t>(b, n));
        },
        receivers_.back().get());
  }
  void Close() override {
    if (t_) rda_transport_close(t_);
  }
  bool GetStats(TransportStats* out) const override {
    rda_transport_stats s{};
    s.struct_size = sizeof(s);
    if (!t_ || !out || !rda_transport_get_stats(t_, &s)) return false;
    *out = {s.bandwidth_bps, s.rtt_us, s.queued_bytes, s.bytes_in_flight, s.packets_sent, s.packets_lost};
    return true;
  }

 private:
  rda_transport* ReleaseNativeHandle() override { return std::exchange(t_, nullptr); }

  rda_transport* t_;
  std::mutex mu_;
  std::vector<std::unique_ptr<ReceiveFn>> receivers_;  // kept: one may be running
};

struct VtableReceiver {
  rda_receive_fn fn;
  void* ctx;
};

inline const rda_transport_vtable& SubclassVtable() {
  static const rda_transport_vtable v = [] {
    rda_transport_vtable t{};
    t.struct_size = sizeof(t);
    t.send = [](void* self, rda_channel ch, const uint8_t* b, size_t n) -> rda_status {
      return static_cast<Transport*>(self)->Send(static_cast<Channel>(ch), std::span<const uint8_t>(b, n))
                 ? RDA_OK
                 : RDA_ERR_CLOSED;
    };
    t.set_receiver = [](void* self, rda_receive_fn fn, void* ctx) {
      if (!fn) {
        static_cast<Transport*>(self)->SetReceiver(nullptr);
        return;
      }
      static_cast<Transport*>(self)->SetReceiver([fn, ctx](Channel ch, std::span<const uint8_t> b) {
        fn(ctx, static_cast<rda_channel>(ch), b.data(), b.size());
      });
    };
    t.close = [](void* self) { static_cast<Transport*>(self)->Close(); };
    t.get_stats = [](void* self, rda_transport_stats* out) -> int {
      TransportStats s;
      if (!static_cast<Transport*>(self)->GetStats(&s)) return 0;
      out->bandwidth_bps = s.bandwidth_bps;
      out->rtt_us = s.rtt_us;
      out->queued_bytes = s.queued_bytes;
      out->bytes_in_flight = s.bytes_in_flight;
      out->packets_sent = s.packets_sent;
      out->packets_lost = s.packets_lost;
      return 1;
    };
    t.destroy = [](void* self) { delete static_cast<Transport*>(self); };
    return t;
  }();
  return v;
}

// Hands a Transport to the C side, which then owns it.
inline rda_transport* IntoHandle(std::unique_ptr<Transport> t) {
  if (!t) return nullptr;
  if (rda_transport* h = t->ReleaseNativeHandle()) return h;  // wrapper freed here
  rda_transport* out = nullptr;
  if (rda_transport_custom(&SubclassVtable(), t.release(), &out) != RDA_OK) return nullptr;
  return out;
}

// Call as Native(rda_x(..., &t), &t, error): `t` is read after the call.
inline std::unique_ptr<Transport> Native(rda_status s, rda_transport* const* t, std::string* error) {
  if (s != RDA_OK) {
    SetError(error);
    return nullptr;
  }
  return std::make_unique<NativeTransport>(*t);
}

using TransportPair = std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>>;

}  // namespace detail

// Two connected in-process endpoints.
inline detail::TransportPair CreateLocalPipe() {
  rda_transport *a = nullptr, *b = nullptr;
  if (rda_pipe_create(&a, &b) != RDA_OK) return {};
  return {std::make_unique<detail::NativeTransport>(a), std::make_unique<detail::NativeTransport>(b)};
}

// TCP: listen blocks until one peer connects. Fixed bitrate.
inline std::unique_ptr<Transport> ListenTcp(uint16_t port, std::string* error = nullptr) {
  rda_transport* t = nullptr;
  return detail::Native(rda_tcp_listen(port, &t), &t, error);
}
inline std::unique_ptr<Transport> ConnectTcp(const std::string& host, uint16_t port, std::string* error = nullptr) {
  rda_transport* t = nullptr;
  return detail::Native(rda_tcp_connect(host.c_str(), port, &t), &t, error);
}

// UDP with loss recovery and congestion control (adaptive bitrate). Listen
// blocks until the peer's first packet arrives.
inline std::unique_ptr<Transport> ListenUdp(uint16_t port, std::string* error = nullptr) {
  rda_transport* t = nullptr;
  return detail::Native(rda_udp_listen(port, &t), &t, error);
}
inline std::unique_ptr<Transport> ConnectUdp(const std::string& host, uint16_t port, std::string* error = nullptr) {
  rda_transport* t = nullptr;
  return detail::Native(rda_udp_connect(host.c_str(), port, &t), &t, error);
}

struct NetworkConditions {
  int64_t bandwidth_bps = 0;  // 0 = unlimited
  int32_t delay_ms = 0;       // one way
  int32_t jitter_ms = 0;
  double loss = 0;
  int32_t queue_ms = 200;
  std::vector<std::pair<int64_t, int64_t>> bandwidth_schedule = {};  // {ms after creation, bps}
  uint32_t seed = 1;
};

// A simulated path running the UDP protocol, for tests and benchmarks.
inline detail::TransportPair CreateSimulatedNetwork(const NetworkConditions& host_to_viewer,
                                                    const NetworkConditions& viewer_to_host,
                                                    bool congestion_control = true) {
  auto conv = [](const NetworkConditions& n, std::vector<rda_bandwidth_step>& steps) {
    rda_network_conditions c;
    rda_network_conditions_default(&c);
    c.bandwidth_bps = n.bandwidth_bps;
    c.delay_ms = n.delay_ms;
    c.jitter_ms = n.jitter_ms;
    c.loss = n.loss;
    c.queue_ms = n.queue_ms;
    c.seed = n.seed;
    for (const auto& [ms, bps] : n.bandwidth_schedule) steps.push_back({ms, bps});
    c.schedule = steps.data();
    c.schedule_len = steps.size();
    return c;
  };
  std::vector<rda_bandwidth_step> s1, s2;
  const rda_network_conditions a = conv(host_to_viewer, s1), b = conv(viewer_to_host, s2);
  rda_transport *h = nullptr, *v = nullptr;
  if (rda_simulated_network_create(&a, &b, congestion_control ? 1 : 0, &h, &v) != RDA_OK) return {};
  return {std::make_unique<detail::NativeTransport>(h), std::make_unique<detail::NativeTransport>(v)};
}

// Recordings (.rdstream): a host transport that writes the stream to a file,
// and reading one back for Viewer::FeedRecording().
inline std::unique_ptr<Transport> CreateRecordingTransport(const std::string& path, std::string* error = nullptr) {
  rda_transport* t = nullptr;
  return detail::Native(rda_recording_create(path.c_str(), &t), &t, error);
}

inline bool ReadRecording(const std::string& path, std::vector<uint8_t>& out, std::string* error = nullptr) {
  uint8_t* bytes = nullptr;
  size_t n = 0;
  if (rda_recording_read(path.c_str(), &bytes, &n) != RDA_OK) return detail::SetError(error);
  out.assign(bytes, bytes + n);
  rda_free(bytes);
  return true;
}

// A deterministic synthetic desktop. Renders frame `index` into `bgra`
// (resized to w * h * 4); returns what differs from frame index - 1. A
// buffer of the wrong size is rendered from scratch.
inline std::vector<Rect> RenderTestPattern(uint64_t index, int32_t w, int32_t h, std::vector<uint8_t>& bgra) {
  const size_t size = static_cast<size_t>(w) * h * 4;
  const bool incremental = bgra.size() == size;
  if (!incremental) bgra.assign(size, 0);
  rda_rect rects[16];
  size_t n = 0;
  if (rda_test_pattern_render(index, w, h, bgra.data(), incremental, rects, 16, &n) != RDA_OK) return {};
  std::vector<Rect> out;
  for (size_t i = 0; i < n && i < 16; ++i) out.push_back({rects[i].x, rects[i].y, rects[i].w, rects[i].h});
  if (n > 16) out = {{0, 0, w, h}};
  return out;
}

// ---- host -------------------------------------------------------------------

// Defaults are the recommended values; field meanings as in rda_host_config.
struct HostConfig {
  int32_t fps = 60;
  int64_t bitrate_bps = 8'000'000;
  int32_t min_qp = 24;
  std::string encoder;  // empty = automatic
  int32_t capture_width = 0;
  int32_t capture_height = 0;
  bool capture_cursor = false;
  bool cursor_channel = true;
  int32_t cursor_poll_hz = 120;
  bool lossless = true;
  int32_t lossless_max_colors = 256;
  int32_t lossless_max_colors_hot = 4;
  bool detect_moves = true;
  int32_t refine_frames = 8;
  bool tile_cache = true;
  bool refresh = false;
  bool progressive = true;
  bool video_regions = true;
  int32_t video_region_min_qp = 27;
  int64_t video_region_bitrate_bps = 0;
  bool adaptive_bitrate = true;
  int64_t min_bitrate_bps = 300'000;
  int32_t max_queue_ms = 50;
  int32_t peer_timeout_ms = 10'000;
  // Whether a viewer may connect, from its auth token (transport thread).
  // Empty = everyone may.
  std::function<bool(std::span<const uint8_t> token)> auth;
};

struct HostStats {
  uint64_t frames_captured = 0;
  uint64_t frames_skipped = 0;
  uint64_t frames_encoded = 0;
  uint64_t video_frames = 0;
  uint64_t keyframes = 0;
  uint64_t keyframe_requests = 0;
  uint64_t encode_errors = 0;
  uint64_t bytes_sent = 0;
  uint64_t video_bytes = 0;
  uint64_t lossless_bytes = 0;
  uint64_t lossless_tiles = 0;
  uint64_t moved_tiles = 0;
  uint64_t cached_tiles = 0;
  uint64_t refreshed_tiles = 0;
  uint64_t progressive_tiles = 0;
  uint64_t region_pictures = 0;
  uint64_t region_bytes = 0;
  uint64_t regions_opened = 0;
  uint64_t regions_closed = 0;
  uint64_t region_encode_us_total = 0;
  uint64_t frames_dropped_congestion = 0;
  uint64_t frames_resubmitted = 0;
  int64_t target_bitrate_bps = 0;
  int64_t bandwidth_estimate_bps = 0;
  uint32_t rtt_us = 0;
  uint64_t cursor_updates = 0;
  uint64_t cursor_images = 0;
  uint64_t cursor_bytes = 0;
  uint64_t encode_us_total = 0;
  uint64_t lossless_us_total = 0;
};

class Host {
 public:
  using InputHandler = std::function<void(const InputEvent&)>;
  using ErrorHandler = std::function<void(const std::string&)>;

  // Takes ownership of the transport. If the host cannot be created (bad
  // config), every later call fails with that error; see ok().
  explicit Host(std::unique_ptr<Transport> transport, HostConfig config = {})
      : state_(std::make_unique<State>()) {
    rda_host_config c;
    rda_host_config_default(&c);
    c.fps = config.fps;
    c.bitrate_bps = config.bitrate_bps;
    c.min_qp = config.min_qp;
    c.encoder = config.encoder.empty() ? nullptr : config.encoder.c_str();
    c.capture_width = config.capture_width;
    c.capture_height = config.capture_height;
    c.capture_cursor = config.capture_cursor;
    c.cursor_channel = config.cursor_channel;
    c.cursor_poll_hz = config.cursor_poll_hz;
    c.lossless = config.lossless;
    c.lossless_max_colors = config.lossless_max_colors;
    c.lossless_max_colors_hot = config.lossless_max_colors_hot;
    c.detect_moves = config.detect_moves;
    c.refine_frames = config.refine_frames;
    c.tile_cache = config.tile_cache;
    c.refresh = config.refresh;
    c.progressive = config.progressive;
    c.video_regions = config.video_regions;
    c.video_region_min_qp = config.video_region_min_qp;
    c.video_region_bitrate_bps = config.video_region_bitrate_bps;
    c.adaptive_bitrate = config.adaptive_bitrate;
    c.min_bitrate_bps = config.min_bitrate_bps;
    c.max_queue_ms = config.max_queue_ms;
    c.peer_timeout_ms = config.peer_timeout_ms;
    c.user = state_.get();
    state_->auth = std::move(config.auth);
    if (state_->auth) {
      c.on_auth = [](void* u, const uint8_t* token, size_t n) -> int {
        return static_cast<State*>(u)->auth(std::span<const uint8_t>(token, n)) ? 1 : 0;
      };
    }
    c.on_state = [](void* u, rda_state st, rda_status reason) {
      auto* s = static_cast<State*>(u);
      StateHandler fn;
      {
        std::lock_guard<std::mutex> lock(s->mu);
        fn = s->on_state;
      }
      if (fn) fn(static_cast<ConnectionState>(st), reason);
    };
    c.on_message = [](void* u, uint32_t type, const uint8_t* p, size_t n) {
      auto* s = static_cast<State*>(u);
      MessageHandler fn;
      {
        std::lock_guard<std::mutex> lock(s->mu);
        fn = s->on_message;
      }
      if (fn) fn(type, std::span<const uint8_t>(p, n));
    };
    c.on_input = [](void* u, const rda_input_event* ev) {
      auto* s = static_cast<State*>(u);
      InputHandler fn;
      {
        std::lock_guard<std::mutex> lock(s->mu);
        fn = s->on_input;
      }
      if (fn) fn(detail::FromC(*ev));
    };
    c.on_error = [](void* u, rda_status, const char* msg) {
      auto* s = static_cast<State*>(u);
      ErrorHandler fn;
      {
        std::lock_guard<std::mutex> lock(s->mu);
        fn = s->on_error;
      }
      if (fn) fn(msg);
    };
    if (rda_host_create(detail::IntoHandle(std::move(transport)), &c, &h_) != RDA_OK) {
      create_error_ = rda_last_error();
      h_ = nullptr;
    }
  }
  ~Host() { rda_host_destroy(h_); }
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  bool ok() const { return h_ != nullptr; }

  // Input events from the viewer, on the transport thread.
  void SetInputHandler(InputHandler fn) {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->on_input = std::move(fn);
  }
  // Errors on a background thread (encoder, capture, cursor).
  void SetErrorHandler(ErrorHandler fn) {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->on_error = std::move(fn);
  }

  // Connection changes (transport or timer thread). Set it before the
  // viewer can connect, or check state() afterwards.
  void SetStateHandler(StateHandler fn) {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->on_state = std::move(fn);
  }
  ConnectionState state() const {
    return h_ ? static_cast<ConnectionState>(rda_host_get_state(h_)) : ConnectionState::kDisconnected;
  }
  // Application messages from the viewer (transport thread).
  void SetMessageHandler(MessageHandler fn) {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->on_message = std::move(fn);
  }
  // Reliable, ordered, at most kMaxMessageSize; thread-safe.
  bool SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error = nullptr) {
    if (!h_) return Failed(error);
    return rda_host_send_message(h_, type, payload.data(), payload.size()) == RDA_OK || detail::SetError(error);
  }
  bool SendMessage(uint32_t type, std::string_view text, std::string* error = nullptr) {
    return SendMessage(type, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
                       error);
  }

  // Mode 1: capture the main display.
  bool StartScreenCapture(std::string* error = nullptr) {
    if (!h_) return Failed(error);
    return rda_host_start_capture(h_) == RDA_OK || detail::SetError(error);
  }
  void StopScreenCapture() {
    if (h_) rda_host_stop_capture(h_);
  }

  // Mode 2: feed frames yourself. Calls must not overlap.
  bool SubmitFrame(const ImageView& bgra, uint64_t capture_time_us, std::span<const Rect> dirty = {},
                   std::string* error = nullptr) {
    if (!h_) return Failed(error);
    const rda_image img{bgra.data, bgra.width, bgra.height, bgra.stride};
    static_assert(sizeof(Rect) == sizeof(rda_rect));
    std::vector<rda_rect> rects(dirty.size());
    for (size_t i = 0; i < dirty.size(); ++i) rects[i] = {dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h};
    return rda_host_submit_frame(h_, &img, capture_time_us, rects.data(), rects.size()) == RDA_OK ||
           detail::SetError(error);
  }

  // The cursor, when you feed frames yourself. Thread-safe.
  void SetCursor(int32_t x, int32_t y, bool visible = true, uint64_t time_us = 0) {
    if (h_) rda_host_set_cursor(h_, x, y, visible ? 1 : 0, time_us);
  }
  void SetCursorImage(const CursorImage& image) {
    if (!h_ || image.bgra.size() != static_cast<size_t>(image.width) * image.height * 4) return;
    const rda_cursor_image ci{image.width, image.height, image.hotspot_x, image.hotspot_y, image.bgra.data()};
    rda_host_set_cursor_image(h_, &ci);
  }

  void RequestKeyframe() {
    if (h_) rda_host_request_keyframe(h_);
  }

  std::string encoder_name() const {
    if (!h_) return {};
    char buf[64];
    rda_host_encoder_name(h_, buf, sizeof(buf));
    return buf;
  }

  HostStats stats() const {
    rda_host_stats s{};
    s.struct_size = sizeof(s);
    if (!h_ || rda_host_get_stats(h_, &s) != RDA_OK) return {};
    HostStats o;
    o.frames_captured = s.frames_captured;
    o.frames_skipped = s.frames_skipped;
    o.frames_encoded = s.frames_encoded;
    o.video_frames = s.video_frames;
    o.keyframes = s.keyframes;
    o.keyframe_requests = s.keyframe_requests;
    o.encode_errors = s.encode_errors;
    o.bytes_sent = s.bytes_sent;
    o.video_bytes = s.video_bytes;
    o.lossless_bytes = s.lossless_bytes;
    o.lossless_tiles = s.lossless_tiles;
    o.moved_tiles = s.moved_tiles;
    o.cached_tiles = s.cached_tiles;
    o.refreshed_tiles = s.refreshed_tiles;
    o.progressive_tiles = s.progressive_tiles;
    o.region_pictures = s.region_pictures;
    o.region_bytes = s.region_bytes;
    o.regions_opened = s.regions_opened;
    o.regions_closed = s.regions_closed;
    o.region_encode_us_total = s.region_encode_us_total;
    o.frames_dropped_congestion = s.frames_dropped_congestion;
    o.frames_resubmitted = s.frames_resubmitted;
    o.target_bitrate_bps = s.target_bitrate_bps;
    o.bandwidth_estimate_bps = s.bandwidth_estimate_bps;
    o.rtt_us = s.rtt_us;
    o.cursor_updates = s.cursor_updates;
    o.cursor_images = s.cursor_images;
    o.cursor_bytes = s.cursor_bytes;
    o.encode_us_total = s.encode_us_total;
    o.lossless_us_total = s.lossless_us_total;
    return o;
  }

  // The C handle, for calling rda_host_* functions directly.
  rda_host* handle() const { return h_; }

 private:
  struct State {
    std::mutex mu;
    InputHandler on_input;
    ErrorHandler on_error;
    StateHandler on_state;
    MessageHandler on_message;
    std::function<bool(std::span<const uint8_t>)> auth;
  };

  bool Failed(std::string* error) const {
    if (error) *error = create_error_;
    return false;
  }

  std::unique_ptr<State> state_;  // outlives h_ (declared first, destroyed last)
  rda_host* h_ = nullptr;
  std::string create_error_;
};

// ---- viewer -----------------------------------------------------------------

struct FrameInfo {
  uint64_t frame_id = 0;
  uint64_t capture_time_us = 0;  // host clock
};

struct ViewerStats {
  uint64_t frames_decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t bytes_received = 0;
  uint64_t decode_us_total = 0;
  uint64_t keyframe_requests = 0;
};

// Handlers run on the transport thread, one at a time (on_state may also
// come from the library's timer thread); pointers and views are valid only
// during the call.
struct ViewerHandlers {
  std::function<void(int32_t width, int32_t height)> on_stream_info;
  std::function<void(const ImageView& bgra, std::span<const Rect> changed, const FrameInfo& info)> on_frame;
  std::function<void(const CursorState& state)> on_cursor;
  StateHandler on_state;
  MessageHandler on_message;
};

struct ViewerOptions {
  int32_t peer_timeout_ms = 10'000;  // 0 = never time out
  std::vector<uint8_t> auth_token;   // for the host's auth hook, at most 4096 bytes
};

class Viewer {
 public:
  // Takes ownership of the transport (may be null to play recordings).
  // Returns null and sets `error` if no HEVC decoder is available.
  static std::unique_ptr<Viewer> Create(std::unique_ptr<Transport> transport, ViewerHandlers handlers,
                                        std::string* error = nullptr) {
    return Create(std::move(transport), std::move(handlers), ViewerOptions{}, error);
  }
  static std::unique_ptr<Viewer> Create(std::unique_ptr<Transport> transport, ViewerHandlers handlers,
                                        const ViewerOptions& options, std::string* error = nullptr) {
    std::unique_ptr<Viewer> v(new Viewer());
    v->state_->h = std::move(handlers);
    rda_viewer_config c;
    rda_viewer_config_default(&c);
    c.user = v->state_.get();
    c.peer_timeout_ms = options.peer_timeout_ms;
    c.auth_token = options.auth_token.data();
    c.auth_token_len = options.auth_token.size();
    if (v->state_->h.on_state) {
      c.on_state = [](void* u, rda_state st, rda_status reason) {
        static_cast<State*>(u)->h.on_state(static_cast<ConnectionState>(st), reason);
      };
    }
    if (v->state_->h.on_message) {
      c.on_message = [](void* u, uint32_t type, const uint8_t* p, size_t n) {
        static_cast<State*>(u)->h.on_message(type, std::span<const uint8_t>(p, n));
      };
    }
    if (v->state_->h.on_stream_info) {
      c.on_stream_info = [](void* u, int32_t w, int32_t h) { static_cast<State*>(u)->h.on_stream_info(w, h); };
    }
    if (v->state_->h.on_frame) {
      c.on_frame = [](void* u, const rda_image* img, const rda_rect* changed, size_t n, const rda_frame_info* info) {
        auto* s = static_cast<State*>(u);
        s->changed.resize(n);
        for (size_t i = 0; i < n; ++i) s->changed[i] = {changed[i].x, changed[i].y, changed[i].w, changed[i].h};
        s->h.on_frame(ImageView{img->data, img->width, img->height, img->stride}, s->changed,
                      FrameInfo{info->frame_id, info->capture_time_us});
      };
    }
    if (v->state_->h.on_cursor) {
      c.on_cursor = [](void* u, const rda_cursor_state* cs) {
        auto* s = static_cast<State*>(u);
        if (cs->image && cs->image_id != s->cursor_id) {
          const rda_cursor_image& i = *cs->image;
          s->cursor.width = i.width;
          s->cursor.height = i.height;
          s->cursor.hotspot_x = i.hotspot_x;
          s->cursor.hotspot_y = i.hotspot_y;
          s->cursor.bgra.assign(i.bgra, i.bgra + static_cast<size_t>(i.width) * i.height * 4);
          s->cursor_id = cs->image_id;
        }
        CursorState st;
        st.x = cs->x;
        st.y = cs->y;
        st.visible = cs->visible != 0;
        st.time_us = cs->time_us;
        st.image_id = cs->image_id;
        st.image_changed = cs->image_changed != 0;
        st.image = cs->image ? &s->cursor : nullptr;
        s->h.on_cursor(st);
      };
    }
    if (rda_viewer_create(detail::IntoHandle(std::move(transport)), &c, &v->v_) != RDA_OK) {
      detail::SetError(error);
      return nullptr;
    }
    return v;
  }

  ~Viewer() { rda_viewer_destroy(v_); }
  Viewer(const Viewer&) = delete;
  Viewer& operator=(const Viewer&) = delete;

  void SendInput(const InputEvent& ev) {
    const rda_input_event c = detail::ToC(ev);
    rda_viewer_send_input(v_, &c);
  }
  void RequestKeyframe() { rda_viewer_request_keyframe(v_); }

  ConnectionState state() const { return static_cast<ConnectionState>(rda_viewer_get_state(v_)); }
  bool SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error = nullptr) {
    return rda_viewer_send_message(v_, type, payload.data(), payload.size()) == RDA_OK || detail::SetError(error);
  }
  bool SendMessage(uint32_t type, std::string_view text, std::string* error = nullptr) {
    return SendMessage(type, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
                       error);
  }
  // Decodes a recording synchronously on this thread.
  void FeedRecording(std::span<const uint8_t> bytes) { rda_viewer_feed_recording(v_, bytes.data(), bytes.size()); }

  std::string decoder_name() const {
    char buf[64];
    rda_viewer_decoder_name(v_, buf, sizeof(buf));
    return buf;
  }

  ViewerStats stats() const {
    rda_viewer_stats s{};
    s.struct_size = sizeof(s);
    if (rda_viewer_get_stats(v_, &s) != RDA_OK) return {};
    return {s.frames_decoded, s.decode_errors, s.bytes_received, s.decode_us_total, s.keyframe_requests};
  }

  rda_viewer* handle() const { return v_; }

 private:
  struct State {
    ViewerHandlers h;
    std::vector<Rect> changed;
    CursorImage cursor;
    uint64_t cursor_id = 0;
  };

  Viewer() : state_(std::make_unique<State>()) {}

  std::unique_ptr<State> state_;  // outlives v_
  rda_viewer* v_ = nullptr;
};

}  // namespace rdaccel
