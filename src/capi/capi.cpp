// The public C ABI (include/rdaccel/rdaccel.h), implemented on the internal
// C++ API (core/rdacore/). Every entry point validates its arguments, accepts
// structs from older headers (struct_size), and turns exceptions into status
// codes: nothing C++ crosses this boundary.

#include "rdaccel/rdaccel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "capture/capturer.h"
#include "capture/synthetic_capturer.h"
#include "log/log.h"
#include "platform.h"
#include "rdacore/rdacore.h"

namespace {

// ---- status and errors ----------------------------------------------------

thread_local std::string g_last_error;

rda_status Fail(rda_status s, std::string msg) {
  g_last_error = std::move(msg);
  return s;
}

bool Contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// Internal errors are text; pick the closest status code.
rda_status Classify(const std::string& msg, rda_status fallback) {
  if (Contains(msg, "already running")) return RDA_ERR_BUSY;
  if (Contains(msg, "ermission")) return RDA_ERR_PERMISSION;
  if (msg.rfind("disconnected", 0) == 0 || Contains(msg, "connection closed")) return RDA_ERR_CLOSED;
  if (Contains(msg, "reserved") || Contains(msg, "larger than")) return RDA_ERR_INVALID_ARG;
  if (Contains(msg, "not connected yet")) return RDA_ERR_BUSY;
  return fallback;
}

#define RDA_GUARD_BEGIN try {
#define RDA_GUARD_END                                              \
  }                                                                \
  catch (const std::bad_alloc&) {                                  \
    return Fail(RDA_ERR_NO_MEMORY, "out of memory");               \
  }                                                                \
  catch (const std::exception& e) {                                \
    return Fail(RDA_ERR_INVALID_ARG, e.what());                    \
  }                                                                \
  catch (...) {                                                    \
    return Fail(RDA_ERR_INVALID_ARG, "unexpected internal error"); \
  }

#define RDA_GUARD_END_VOID \
  }                        \
  catch (...) {            \
    return;                \
  }

// ---- versioned structs ----------------------------------------------------

// Copies a caller's struct over `out` (already holding defaults), up to the
// smaller of the two sizes, so structs from older headers keep working.
template <class T>
bool ReadStruct(const T* in, T& out) {
  if (!in || in->struct_size < sizeof(uint32_t)) return false;
  std::memcpy(&out, in, std::min<size_t>(in->struct_size, sizeof(T)));
  out.struct_size = sizeof(T);
  return true;
}

template <class T>
bool WriteStruct(const T& value, T* out) {
  if (!out || out->struct_size < sizeof(uint32_t)) return false;
  const uint32_t n = out->struct_size;
  std::memcpy(out, &value, std::min<size_t>(n, sizeof(T)));
  out->struct_size = n;
  return true;
}

size_t CopyName(const std::string& s, char* buf, size_t size) {
  if (buf && size > 0) {
    const size_t n = std::min(s.size(), size - 1);
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
  }
  return s.size();
}

// ---- conversions ----------------------------------------------------------

rdacore::TransportStats FromC(const rda_transport_stats& s) {
  rdacore::TransportStats o;
  o.bandwidth_bps = s.bandwidth_bps;
  o.rtt_us = s.rtt_us;
  o.queued_bytes = s.queued_bytes;
  o.bytes_in_flight = s.bytes_in_flight;
  o.packets_sent = s.packets_sent;
  o.packets_lost = s.packets_lost;
  o.packets_spurious = s.packets_spurious;
  o.fec_parity_sent = s.fec_parity_sent;
  o.fec_recovered = s.fec_recovered;
  return o;
}

rda_transport_stats ToC(const rdacore::TransportStats& s) {
  rda_transport_stats o{};
  o.struct_size = sizeof(o);
  o.bandwidth_bps = s.bandwidth_bps;
  o.rtt_us = s.rtt_us;
  o.queued_bytes = s.queued_bytes;
  o.bytes_in_flight = s.bytes_in_flight;
  o.packets_sent = s.packets_sent;
  o.packets_lost = s.packets_lost;
  o.packets_spurious = s.packets_spurious;
  o.fec_parity_sent = s.fec_parity_sent;
  o.fec_recovered = s.fec_recovered;
  return o;
}

rda_input_event ToC(const rdacore::InputEvent& e) {
  rda_input_event o{};
  o.struct_size = sizeof(o);
  o.kind = static_cast<rda_input_kind>(e.kind);
  o.x = e.x;
  o.y = e.y;
  o.delta_x = e.delta_x;
  o.delta_y = e.delta_y;
  o.code = e.code;
  o.modifiers = e.modifiers;
  return o;
}

bool FromC(const rda_input_event& e, rdacore::InputEvent& o) {
  if (e.kind < RDA_INPUT_MOUSE_MOVE || e.kind > RDA_INPUT_TEXT) return false;
  if (e.kind == RDA_INPUT_TEXT && !rdacore::IsUnicodeScalar(e.code)) return false;
  o.kind = static_cast<rdacore::InputKind>(e.kind);
  o.x = e.x;
  o.y = e.y;
  o.delta_x = e.delta_x;
  o.delta_y = e.delta_y;
  o.code = e.code;
  o.modifiers = e.modifiers;
  return true;
}

rdacore::NetworkConditions FromC(const rda_network_conditions& c) {
  rdacore::NetworkConditions o;
  o.bandwidth_bps = c.bandwidth_bps;
  o.delay_ms = c.delay_ms;
  o.jitter_ms = c.jitter_ms;
  o.loss = c.loss;
  o.queue_ms = c.queue_ms;
  o.seed = c.seed;
  for (size_t i = 0; c.schedule && i < c.schedule_len; ++i) {
    o.bandwidth_schedule.emplace_back(c.schedule[i].at_ms, c.schedule[i].bandwidth_bps);
  }
  return o;
}

bool ValidChannel(rda_channel ch) { return ch >= RDA_CHANNEL_CONTROL && ch < RDA_CHANNEL_COUNT; }

static_assert(RDA_STATE_CONNECTING == static_cast<int>(rdacore::ConnectionState::kConnecting) &&
              RDA_STATE_CONNECTED == static_cast<int>(rdacore::ConnectionState::kConnected) &&
              RDA_STATE_DISCONNECTED == static_cast<int>(rdacore::ConnectionState::kDisconnected));
static_assert(RDA_CHANNEL_COUNT == rdacore::kChannelCount);
static_assert(RDA_MSG_CLIPBOARD_TEXT == rdacore::kClipboardTextMessage &&
              RDA_MSG_USER_FIRST == rdacore::kFirstUserMessage);
static_assert(RDA_KEYCODE_NONE == rdacore::kNoKeyCode);

rda_state ToC(rdacore::ConnectionState s) { return static_cast<rda_state>(s); }

rda_status ToC(rdacore::ConnectionError e) {
  switch (e) {
    case rdacore::ConnectionError::kNone: return RDA_OK;
    case rdacore::ConnectionError::kClosed: return RDA_ERR_CLOSED;
    case rdacore::ConnectionError::kTimeout: return RDA_ERR_TIMEOUT;
    case rdacore::ConnectionError::kProtocol: return RDA_ERR_PROTOCOL;
    case rdacore::ConnectionError::kRejected: return RDA_ERR_AUTH;
  }
  return RDA_ERR_CLOSED;
}

rda_status SendMessageStatus(bool ok, const std::string& err) {
  return ok ? RDA_OK : Fail(Classify(err, RDA_ERR_CLOSED), err);
}

// ---- custom transports ----------------------------------------------------

// An application transport behind the C vtable, seen by the library as an
// ordinary rdacore::Transport.
class VtableTransport final : public rdacore::Transport {
 public:
  VtableTransport(const rda_transport_vtable& v, void* self) : v_(v), self_(self) {}
  ~VtableTransport() override { v_.destroy(self_); }

  bool Send(rdacore::Channel ch, std::span<const uint8_t> bytes) override {
    return v_.send(self_, static_cast<rda_channel>(ch), bytes.data(), bytes.size()) == RDA_OK;
  }

  void SetReceiver(ReceiveFn fn) override {
    if (!fn) {
      v_.set_receiver(self_, nullptr, nullptr);
      return;
    }
    // Earlier receivers stay alive: the transport may still be calling one.
    std::lock_guard<std::mutex> lock(mu_);
    receivers_.push_back(std::make_unique<ReceiveFn>(std::move(fn)));
    v_.set_receiver(self_, &Thunk, receivers_.back().get());
  }

  void Close() override {
    if (v_.close) v_.close(self_);
  }

  bool GetStats(rdacore::TransportStats* out) const override {
    if (!v_.get_stats || !out) return false;
    rda_transport_stats s{};
    s.struct_size = sizeof(s);
    if (!v_.get_stats(self_, &s)) return false;
    *out = FromC(s);
    return true;
  }

 private:
  static void Thunk(void* ctx, rda_channel ch, const uint8_t* bytes, size_t n) {
    if (!ValidChannel(ch) || (!bytes && n)) return;
    (*static_cast<ReceiveFn*>(ctx))(static_cast<rdacore::Channel>(ch), std::span<const uint8_t>(bytes, n));
  }

  rda_transport_vtable v_;
  void* self_;
  std::mutex mu_;
  std::vector<std::unique_ptr<ReceiveFn>> receivers_;
};

}  // namespace

struct rda_transport {
  std::unique_ptr<rdacore::Transport> t;
  bool network = false;  // TCP / UDP: plaintext only when allowed
};

namespace {

rda_status Wrap(std::unique_ptr<rdacore::Transport> t, rda_transport** out, const std::string& err,
                rda_status fail, bool network = false) {
  if (!t) return Fail(fail, err.empty() ? "transport could not be created" : err);
  *out = new rda_transport{std::move(t), network};
  return RDA_OK;
}

}  // namespace

extern "C" {

// ---- version, errors, logging ---------------------------------------------

uint32_t rda_version(void) { return RDA_VERSION; }
const char* rda_version_string(void) { return RDACCEL_VERSION_STRING; }
const char* rda_platform_name(void) { return rdacore::PlatformName(); }
uint64_t rda_now_us(void) { return rdacore::NowMicros(); }

rda_status rda_list_displays(rda_display_info* out, size_t capacity, size_t* count) {
  RDA_GUARD_BEGIN
  if (!count || (capacity > 0 && !out)) return Fail(RDA_ERR_INVALID_ARG, "count (and out with a capacity) required");
  const size_t stride = capacity > 0 ? out[0].struct_size : 0;
  if (capacity > 0 && stride < sizeof(uint32_t)) return Fail(RDA_ERR_INVALID_ARG, "out[0].struct_size is required");
  std::vector<rdacore::DisplayInfo> all;
  std::string err;
  if (!rdacore::ListDisplays(all, &err)) return Fail(RDA_ERR_UNSUPPORTED, err);
  *count = all.size();
  auto* base = reinterpret_cast<uint8_t*>(out);
  for (size_t i = 0; i < all.size() && i < capacity; ++i) {
    rda_display_info d{};
    d.struct_size = static_cast<uint32_t>(std::min(stride, sizeof(d)));
    d.index = all[i].index;
    d.x = all[i].x;
    d.y = all[i].y;
    d.width = all[i].width;
    d.height = all[i].height;
    d.primary = all[i].primary ? 1 : 0;
    CopyName(all[i].name, d.name, sizeof(d.name));
    auto* entry = base + i * stride;
    std::memset(entry, 0, stride);
    std::memcpy(entry, &d, std::min(stride, sizeof(d)));
  }
  return RDA_OK;
  RDA_GUARD_END
}

const char* rda_status_str(rda_status s) {
  switch (s) {
    case RDA_OK: return "RDA_OK";
    case RDA_ERR_INVALID_ARG: return "RDA_ERR_INVALID_ARG";
    case RDA_ERR_NO_MEMORY: return "RDA_ERR_NO_MEMORY";
    case RDA_ERR_UNSUPPORTED: return "RDA_ERR_UNSUPPORTED";
    case RDA_ERR_PERMISSION: return "RDA_ERR_PERMISSION";
    case RDA_ERR_CODEC: return "RDA_ERR_CODEC";
    case RDA_ERR_TRANSPORT: return "RDA_ERR_TRANSPORT";
    case RDA_ERR_TIMEOUT: return "RDA_ERR_TIMEOUT";
    case RDA_ERR_CLOSED: return "RDA_ERR_CLOSED";
    case RDA_ERR_AUTH: return "RDA_ERR_AUTH";
    case RDA_ERR_PROTOCOL: return "RDA_ERR_PROTOCOL";
    case RDA_ERR_BUSY: return "RDA_ERR_BUSY";
    case RDA_ERR_IO: return "RDA_ERR_IO";
  }
  return "RDA_ERR_UNKNOWN";
}

const char* rda_last_error(void) { return g_last_error.c_str(); }

void rda_set_log(rda_log_fn fn, void* user, rda_log_level max_level) {
  struct Target {
    rda_log_fn fn;
    void* user;
  };
  // Each sink gets its own small record, kept until exit: a message may
  // still be on its way to the previous one while it is being replaced.
  if (!fn) {
    rdacore::log::SetSink(nullptr, nullptr, rdacore::log::Level::kError);
    return;
  }
  static std::mutex mu;
  static std::vector<std::unique_ptr<Target>> targets;
  Target* target = nullptr;
  try {
    std::lock_guard<std::mutex> lock(mu);
    targets.push_back(std::make_unique<Target>(Target{fn, user}));
    target = targets.back().get();
  } catch (...) {
    return;
  }
  rdacore::log::SetSink(
      [](void* u, int level, const char* msg) {
        auto* t = static_cast<Target*>(u);
        t->fn(t->user, static_cast<rda_log_level>(level), msg);
      },
      target, static_cast<rdacore::log::Level>(std::clamp<int>(max_level, 0, 3)));
}

// ---- cursor, input ----------------------------------------------------------

void rda_draw_cursor(const rda_cursor_image* c, int32_t x, int32_t y, uint8_t* bgra, int32_t width,
                     int32_t height, int32_t stride) {
  RDA_GUARD_BEGIN
  if (!c || !c->bgra || !bgra || c->width <= 0 || c->height <= 0 || c->width > 256 || c->height > 256) return;
  rdacore::CursorImage img;
  img.width = c->width;
  img.height = c->height;
  img.hotspot_x = c->hotspot_x;
  img.hotspot_y = c->hotspot_y;
  img.bgra.assign(c->bgra, c->bgra + static_cast<size_t>(c->width) * c->height * 4);
  rdacore::DrawCursor(img, x, y, bgra, width, height, stride);
  RDA_GUARD_END_VOID
}

void rda_input_event_default(rda_input_event* ev) {
  if (!ev) return;
  *ev = rda_input_event{};
  ev->struct_size = sizeof(*ev);
  ev->kind = RDA_INPUT_MOUSE_MOVE;
}

// ---- input injection --------------------------------------------------------

struct rda_injector {
  std::unique_ptr<rdacore::platform::IInputInjector> impl;
};

void rda_injector_config_default(rda_injector_config* config) {
  if (!config) return;
  *config = rda_injector_config{};
  config->struct_size = sizeof(*config);
}

rda_status rda_injector_create(const rda_injector_config* config, rda_injector** out) {
  RDA_GUARD_BEGIN
  if (!out) return Fail(RDA_ERR_INVALID_ARG, "out is required");
  *out = nullptr;
  rda_injector_config c;
  rda_injector_config_default(&c);
  if (config && !ReadStruct(config, c)) return Fail(RDA_ERR_INVALID_ARG, "config needs struct_size");
  if (c.display_index < 0 || c.source_width < 0 || c.source_height < 0) {
    return Fail(RDA_ERR_INVALID_ARG, "negative display index or source size");
  }
  std::string err;
  auto impl = rdacore::platform::CreateInputInjector({c.display_index, c.source_width, c.source_height}, &err);
  if (!impl) return Fail(Classify(err, RDA_ERR_UNSUPPORTED), err);
  *out = new rda_injector{std::move(impl)};
  return RDA_OK;
  RDA_GUARD_END
}

void rda_injector_destroy(rda_injector* injector) { delete injector; }

rda_status rda_injector_inject(rda_injector* injector, const rda_input_event* ev) {
  RDA_GUARD_BEGIN
  if (!injector || !ev) return Fail(RDA_ERR_INVALID_ARG, "injector and event are required");
  rda_input_event e;
  rda_input_event_default(&e);
  rdacore::InputEvent ie;
  if (!ReadStruct(ev, e) || !FromC(e, ie)) return Fail(RDA_ERR_INVALID_ARG, "bad input event");
  std::string err;
  if (!injector->impl->Inject(ie, &err)) return Fail(RDA_ERR_UNSUPPORTED, err);
  return RDA_OK;
  RDA_GUARD_END
}

// ---- keyboard input sources ------------------------------------------------

namespace {

rda_input_source_info ToC(const rdacore::platform::InputSourceInfo& s) {
  rda_input_source_info o{};
  o.struct_size = sizeof(o);
  CopyName(s.id, o.id, sizeof(o.id));
  CopyName(s.name, o.name, sizeof(o.name));
  CopyName(s.language, o.language, sizeof(o.language));
  o.input_method = s.input_method ? 1 : 0;
  o.ascii_capable = s.ascii_capable ? 1 : 0;
  return o;
}

}  // namespace

rda_status rda_list_input_sources(rda_input_source_info* out, size_t capacity, size_t* count) {
  RDA_GUARD_BEGIN
  if (!count || (capacity > 0 && !out)) return Fail(RDA_ERR_INVALID_ARG, "count (and out with a capacity) required");
  const size_t stride = capacity > 0 ? out[0].struct_size : 0;
  if (capacity > 0 && stride < sizeof(uint32_t)) return Fail(RDA_ERR_INVALID_ARG, "out[0].struct_size is required");
  std::vector<rdacore::platform::InputSourceInfo> all;
  std::string err;
  if (!rdacore::platform::ListInputSources(all, &err)) return Fail(RDA_ERR_UNSUPPORTED, err);
  *count = all.size();
  auto* base = reinterpret_cast<uint8_t*>(out);
  for (size_t i = 0; i < all.size() && i < capacity; ++i) {
    rda_input_source_info s = ToC(all[i]);
    s.struct_size = static_cast<uint32_t>(std::min(stride, sizeof(s)));
    auto* entry = base + i * stride;
    std::memset(entry, 0, stride);
    std::memcpy(entry, &s, std::min(stride, sizeof(s)));
  }
  return RDA_OK;
  RDA_GUARD_END
}

rda_status rda_current_input_source(rda_input_source_info* out) {
  RDA_GUARD_BEGIN
  if (!out) return Fail(RDA_ERR_INVALID_ARG, "out is required");
  rdacore::platform::InputSourceInfo cur;
  std::string err;
  if (!rdacore::platform::CurrentInputSource(cur, &err)) return Fail(RDA_ERR_UNSUPPORTED, err);
  if (!WriteStruct(ToC(cur), out)) return Fail(RDA_ERR_INVALID_ARG, "out needs struct_size");
  return RDA_OK;
  RDA_GUARD_END
}

rda_status rda_injector_select_input_source(rda_injector* injector, const char* id) {
  RDA_GUARD_BEGIN
  if (!injector || !id || !*id) return Fail(RDA_ERR_INVALID_ARG, "injector and id are required");
  std::string err;
  if (!injector->impl->SelectInputSource(id, &err)) {
    return Fail(err.find("not enabled") != std::string::npos ? RDA_ERR_INVALID_ARG : RDA_ERR_UNSUPPORTED, err);
  }
  return RDA_OK;
  RDA_GUARD_END
}

// ---- transports -----------------------------------------------------------

rda_status rda_transport_custom(const rda_transport_vtable* vtable, void* self, rda_transport** out) {
  if (!vtable || vtable->struct_size < sizeof(rda_transport_vtable)) {
    return Fail(RDA_ERR_INVALID_ARG, "vtable with struct_size is required");
  }
  const rda_transport_vtable v = *vtable;
  if (!v.destroy) return Fail(RDA_ERR_INVALID_ARG, "vtable needs destroy");
  if (!out || !v.send || !v.set_receiver) {
    v.destroy(self);
    return Fail(RDA_ERR_INVALID_ARG, "out, send and set_receiver are required");
  }
  try {
    *out = new rda_transport{std::make_unique<VtableTransport>(v, self)};
  } catch (...) {
    v.destroy(self);
    return Fail(RDA_ERR_NO_MEMORY, "out of memory");
  }
  return RDA_OK;
}

rda_status rda_pipe_create(rda_transport** a, rda_transport** b) {
  RDA_GUARD_BEGIN
  if (!a || !b) return Fail(RDA_ERR_INVALID_ARG, "a and b are required");
  auto [x, y] = rdacore::CreateLocalPipe();
  *a = new rda_transport{std::move(x)};
  *b = new rda_transport{std::move(y)};
  return RDA_OK;
  RDA_GUARD_END
}

rda_status rda_tcp_connect(const char* host, uint16_t port, rda_transport** out) {
  RDA_GUARD_BEGIN
  if (!host || !out) return Fail(RDA_ERR_INVALID_ARG, "host and out are required");
  std::string err;
  auto t = rdacore::ConnectTcp(host, port, &err);
  return Wrap(std::move(t), out, err, RDA_ERR_TRANSPORT, /*network=*/true);
  RDA_GUARD_END
}

rda_status rda_tcp_bind(uint16_t port, rda_transport** out) {
  RDA_GUARD_BEGIN
  if (!out) return Fail(RDA_ERR_INVALID_ARG, "out is required");
  std::string err;
  auto t = rdacore::BindTcp(port, &err);
  return Wrap(std::move(t), out, err, RDA_ERR_TRANSPORT, /*network=*/true);
  RDA_GUARD_END
}

rda_status rda_udp_connect(const char* host, uint16_t port, rda_transport** out) {
  RDA_GUARD_BEGIN
  if (!host || !out) return Fail(RDA_ERR_INVALID_ARG, "host and out are required");
  std::string err;
  auto t = rdacore::ConnectUdp(host, port, &err);
  return Wrap(std::move(t), out, err, RDA_ERR_TRANSPORT, /*network=*/true);
  RDA_GUARD_END
}

rda_status rda_udp_bind(uint16_t port, rda_transport** out) {
  RDA_GUARD_BEGIN
  if (!out) return Fail(RDA_ERR_INVALID_ARG, "out is required");
  std::string err;
  auto t = rdacore::BindUdp(port, &err);
  return Wrap(std::move(t), out, err, RDA_ERR_TRANSPORT, /*network=*/true);
  RDA_GUARD_END
}

rda_status rda_udp_from_fd(int fd, int viewer_side, rda_transport** out) {
  RDA_GUARD_BEGIN
  if (fd < 0 || !out) return Fail(RDA_ERR_INVALID_ARG, "fd and out are required");
  std::string err;
  auto t = rdacore::UdpFromFd(fd, viewer_side != 0, &err);
  if (!t) return Fail(RDA_ERR_INVALID_ARG, err);
  return Wrap(std::move(t), out, err, RDA_ERR_TRANSPORT, /*network=*/true);
  RDA_GUARD_END
}

void rda_network_conditions_default(rda_network_conditions* c) {
  if (!c) return;
  const rdacore::NetworkConditions d;
  *c = rda_network_conditions{};
  c->struct_size = sizeof(*c);
  c->bandwidth_bps = d.bandwidth_bps;
  c->delay_ms = d.delay_ms;
  c->jitter_ms = d.jitter_ms;
  c->loss = d.loss;
  c->queue_ms = d.queue_ms;
  c->seed = d.seed;
}

rda_status rda_simulated_network_create(const rda_network_conditions* host_to_viewer,
                                        const rda_network_conditions* viewer_to_host,
                                        int congestion_control, rda_transport** host_end,
                                        rda_transport** viewer_end) {
  RDA_GUARD_BEGIN
  if (!host_end || !viewer_end) return Fail(RDA_ERR_INVALID_ARG, "host_end and viewer_end are required");
  rda_network_conditions a, b;
  rda_network_conditions_default(&a);
  rda_network_conditions_default(&b);
  if ((host_to_viewer && !ReadStruct(host_to_viewer, a)) || (viewer_to_host && !ReadStruct(viewer_to_host, b))) {
    return Fail(RDA_ERR_INVALID_ARG, "network conditions need struct_size");
  }
  if (a.loss < 0 || a.loss > 1 || b.loss < 0 || b.loss > 1) return Fail(RDA_ERR_INVALID_ARG, "loss must be 0..1");
  auto [x, y] = rdacore::CreateSimulatedNetwork(FromC(a), FromC(b), congestion_control != 0);
  *host_end = new rda_transport{std::move(x)};
  *viewer_end = new rda_transport{std::move(y)};
  return RDA_OK;
  RDA_GUARD_END
}

rda_status rda_recording_create(const char* path, rda_transport** out) {
  RDA_GUARD_BEGIN
  if (!path || !out) return Fail(RDA_ERR_INVALID_ARG, "path and out are required");
  std::string err;
  auto t = rdacore::CreateRecordingTransport(path, &err);
  return Wrap(std::move(t), out, err, RDA_ERR_IO);
  RDA_GUARD_END
}

rda_status rda_transport_send(rda_transport* t, rda_channel ch, const uint8_t* bytes, size_t n) {
  RDA_GUARD_BEGIN
  if (!t || !ValidChannel(ch) || (!bytes && n)) return Fail(RDA_ERR_INVALID_ARG, "bad transport, channel or bytes");
  if (!t->t->Send(static_cast<rdacore::Channel>(ch), std::span<const uint8_t>(bytes, n))) {
    return Fail(RDA_ERR_CLOSED, "connection closed");
  }
  return RDA_OK;
  RDA_GUARD_END
}

void rda_transport_set_receiver(rda_transport* t, rda_receive_fn fn, void* ctx) {
  RDA_GUARD_BEGIN
  if (!t) return;
  if (!fn) {
    t->t->SetReceiver(nullptr);
    return;
  }
  t->t->SetReceiver([fn, ctx](rdacore::Channel ch, std::span<const uint8_t> b) {
    fn(ctx, static_cast<rda_channel>(ch), b.data(), b.size());
  });
  RDA_GUARD_END_VOID
}

void rda_transport_close(rda_transport* t) {
  RDA_GUARD_BEGIN
  if (t) t->t->Close();
  RDA_GUARD_END_VOID
}

int rda_transport_get_stats(rda_transport* t, rda_transport_stats* out) {
  try {
    rdacore::TransportStats s;
    if (!t || !t->t->GetStats(&s)) return 0;
    return WriteStruct(ToC(s), out) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

void rda_transport_destroy(rda_transport* t) { delete t; }

}  // extern "C"

// ---- host -----------------------------------------------------------------

// Holds the application's callbacks until rda_host_create() or
// rda_viewer_create() has returned, so they always see the handle in the
// caller's variable (a viewer can connect while create is still running).
class CallbackGate {
 public:
  void Wait() {
    if (open_.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> l(mu_);
    cv_.wait(l, [this] { return open_.load(); });
  }
  void Open() {
    {
      std::lock_guard<std::mutex> l(mu_);
      open_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::atomic<bool> open_{false};
};

struct rda_host {
  rda_host_config config;
  std::string encoder;  // storage for config.encoder
  CallbackGate ready;   // before `host`, which runs the callbacks
  std::unique_ptr<rdacore::Host> host;
  ~rda_host() { ready.Open(); }  // let a waiting callback finish first
};

namespace {

rdacore::HostConfig ToHostConfig(const rda_host_config& c) {
  rdacore::HostConfig o;
  o.fps = c.fps;
  o.bitrate_bps = c.bitrate_bps;
  o.min_qp = c.min_qp;
  o.encoder = c.encoder ? c.encoder : "";
  o.capture_width = c.capture_width;
  o.capture_height = c.capture_height;
  o.capture_cursor = c.capture_cursor != 0;
  o.cursor_channel = c.cursor_channel != 0;
  o.cursor_poll_hz = c.cursor_poll_hz;
  o.lossless = c.lossless != 0;
  o.lossless_max_colors = c.lossless_max_colors;
  o.lossless_max_colors_hot = c.lossless_max_colors_hot;
  o.detect_moves = c.detect_moves != 0;
  o.refine_frames = c.refine_frames;
  o.tile_cache = c.tile_cache != 0;
  o.refresh = c.refresh != 0;
  o.progressive = c.progressive != 0;
  o.video_regions = c.video_regions != 0;
  o.video_region_min_qp = c.video_region_min_qp;
  o.video_region_bitrate_bps = c.video_region_bitrate_bps;
  o.adaptive_bitrate = c.adaptive_bitrate != 0;
  o.min_bitrate_bps = c.min_bitrate_bps;
  o.max_queue_ms = c.max_queue_ms;
  o.peer_timeout_ms = c.peer_timeout_ms;
  o.display_index = c.display_index;
  o.keep_serving = c.keep_serving != 0;
  o.capture_own_windows = c.capture_own_windows != 0;
  return o;
}

// Copies the key, or checks that plaintext is acceptable on this transport.
std::string TakeKey(const uint8_t* key, int32_t allow_plaintext, const rda_transport* t,
                    std::vector<uint8_t>& out) {
  if (key) {
    out.assign(key, key + 32);
    return {};
  }
  if (t && t->network && !allow_plaintext) {
    return "TCP/UDP without encryption: set encryption_key (32 bytes, the same on both sides) "
           "or allow_plaintext = 1";
  }
  return {};
}

std::string CheckHostConfig(const rda_host_config& c) {
  if (c.fps < 1 || c.fps > 240) return "fps must be 1..240";
  if (c.bitrate_bps < 50'000) return "bitrate_bps must be at least 50000";
  if (c.min_bitrate_bps < 0) return "min_bitrate_bps must not be negative";
  if (c.min_qp < 0 || c.min_qp > 51 || c.video_region_min_qp < 0 || c.video_region_min_qp > 51) {
    return "QP values must be 0..51";
  }
  if (c.capture_width < 0 || c.capture_height < 0 || (c.capture_width % 2) || (c.capture_height % 2) ||
      ((c.capture_width == 0) != (c.capture_height == 0))) {
    return "capture_width/capture_height must both be 0 or both even and positive";
  }
  if (c.cursor_poll_hz < 1 || c.cursor_poll_hz > 1000) return "cursor_poll_hz must be 1..1000";
  if (c.lossless_max_colors < 1 || c.lossless_max_colors > 256 || c.lossless_max_colors_hot < 0 ||
      c.lossless_max_colors_hot > 256) {
    return "lossless colour limits must be 0..256";
  }
  if (c.refine_frames < 0 || c.max_queue_ms < 1 || c.video_region_bitrate_bps < 0) {
    return "refine_frames, max_queue_ms or video_region_bitrate_bps out of range";
  }
  if (c.peer_timeout_ms < 0) return "peer_timeout_ms must not be negative";
  if (c.display_index < 0) return "display_index must not be negative";
  return {};
}

}  // namespace

extern "C" {

void rda_host_config_default(rda_host_config* c) {
  if (!c) return;
  const rdacore::HostConfig d;
  *c = rda_host_config{};
  c->struct_size = sizeof(*c);
  c->fps = d.fps;
  c->bitrate_bps = d.bitrate_bps;
  c->min_qp = d.min_qp;
  c->encoder = nullptr;
  c->capture_width = d.capture_width;
  c->capture_height = d.capture_height;
  c->capture_cursor = d.capture_cursor;
  c->cursor_channel = d.cursor_channel;
  c->cursor_poll_hz = d.cursor_poll_hz;
  c->lossless = d.lossless;
  c->lossless_max_colors = d.lossless_max_colors;
  c->lossless_max_colors_hot = d.lossless_max_colors_hot;
  c->detect_moves = d.detect_moves;
  c->refine_frames = d.refine_frames;
  c->tile_cache = d.tile_cache;
  c->refresh = d.refresh;
  c->progressive = d.progressive;
  c->video_regions = d.video_regions;
  c->video_region_min_qp = d.video_region_min_qp;
  c->video_region_bitrate_bps = d.video_region_bitrate_bps;
  c->adaptive_bitrate = d.adaptive_bitrate;
  c->min_bitrate_bps = d.min_bitrate_bps;
  c->max_queue_ms = d.max_queue_ms;
  c->peer_timeout_ms = d.peer_timeout_ms;
  c->display_index = d.display_index;
  c->keep_serving = d.keep_serving;
  c->capture_own_windows = d.capture_own_windows;
}

rda_status rda_host_create(rda_transport* transport, const rda_host_config* config, rda_host** out) {
  std::unique_ptr<rda_transport> owned(transport);  // taken over in every case
  RDA_GUARD_BEGIN
  if (!transport || !out) return Fail(RDA_ERR_INVALID_ARG, "transport and out are required");
  auto h = std::make_unique<rda_host>();
  rda_host_config_default(&h->config);
  if (config && !ReadStruct(config, h->config)) return Fail(RDA_ERR_INVALID_ARG, "config needs struct_size");
  if (auto err = CheckHostConfig(h->config); !err.empty()) return Fail(RDA_ERR_INVALID_ARG, err);
  if (h->config.encoder) h->encoder = h->config.encoder;
  h->config.encoder = h->encoder.empty() ? nullptr : h->encoder.c_str();

  rda_host* raw = h.get();
  rdacore::HostConfig hc = ToHostConfig(h->config);
  if (auto err = TakeKey(h->config.encryption_key, h->config.allow_plaintext, owned.get(), hc.encryption_key);
      !err.empty()) {
    return Fail(RDA_ERR_INVALID_ARG, err);
  }
  h->config.encryption_key = nullptr;  // not kept: the caller's memory
  if (raw->config.on_auth) {
    hc.auth = [raw](std::span<const uint8_t> token) {
      raw->ready.Wait();
      return raw->config.on_auth(raw->config.user, token.data(), token.size()) != 0;
    };
  }
  if (raw->config.on_state) {
    hc.on_state = [raw](rdacore::ConnectionState st, rdacore::ConnectionError e) {
      raw->ready.Wait();
      raw->config.on_state(raw->config.user, ToC(st), ToC(e));
    };
  }
  if (raw->config.on_message) {
    hc.on_message = [raw](uint32_t type, std::span<const uint8_t> p) {
      raw->ready.Wait();
      raw->config.on_message(raw->config.user, type, p.data(), p.size());
    };
  }
  h->host = std::make_unique<rdacore::Host>(std::move(owned->t), std::move(hc));
  owned.reset();
  if (raw->config.on_input) {
    raw->host->SetInputHandler([raw](const rdacore::InputEvent& ev) {
      const rda_input_event c = ToC(ev);
      raw->ready.Wait();
      raw->config.on_input(raw->config.user, &c);
    });
  }
  raw->host->SetErrorHandler([raw](const std::string& msg) {
    rdacore::log::Error(msg);
    if (raw->config.on_error) {
      // The contract (rdaccel.h, on_error): RDA_ERR_UNSUPPORTED only for the
      // cursor, which is not fatal; anything else means the host stopped.
      rda_status s = RDA_ERR_CODEC;  // the encoder could not be created
      if (msg.rfind("cursor", 0) == 0) {
        s = RDA_ERR_UNSUPPORTED;
      } else if (msg.rfind("capture stopped", 0) == 0) {
        s = Contains(msg, "ermission") ? RDA_ERR_PERMISSION : RDA_ERR_CLOSED;
      }
      raw->ready.Wait();
      raw->config.on_error(raw->config.user, s, msg.c_str());
    }
  });
  *out = h.release();
  (*out)->ready.Open();
  return RDA_OK;
  RDA_GUARD_END
}

void rda_host_destroy(rda_host* h) { delete h; }

rda_status rda_host_start_capture(rda_host* h) {
  RDA_GUARD_BEGIN
  if (!h) return Fail(RDA_ERR_INVALID_ARG, "host is required");
  std::string err;
  if (!h->host->StartScreenCapture(&err)) return Fail(Classify(err, RDA_ERR_UNSUPPORTED), err);
  return RDA_OK;
  RDA_GUARD_END
}

void rda_host_stop_capture(rda_host* h) {
  RDA_GUARD_BEGIN
  if (h) h->host->StopScreenCapture();
  RDA_GUARD_END_VOID
}

rda_status rda_host_submit_frame(rda_host* h, const rda_image* bgra, uint64_t capture_time_us,
                                 const rda_rect* dirty, size_t dirty_count) {
  RDA_GUARD_BEGIN
  if (!h || !bgra || !bgra->data) return Fail(RDA_ERR_INVALID_ARG, "host and image are required");
  if (bgra->width <= 0 || bgra->height <= 0 || bgra->width > 16384 || bgra->height > 16384 ||
      (bgra->width % 2) || (bgra->height % 2) || bgra->stride < bgra->width * 4) {
    return Fail(RDA_ERR_INVALID_ARG, "image must be even-sized, at most 16384, stride >= width * 4");
  }
  if (!dirty && dirty_count) return Fail(RDA_ERR_INVALID_ARG, "dirty is null but dirty_count is not 0");
  std::vector<rdacore::Rect> rects(dirty_count);
  for (size_t i = 0; i < dirty_count; ++i) rects[i] = {dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h};
  const rdacore::ImageView img{bgra->data, bgra->width, bgra->height, bgra->stride};
  std::string err;
  if (!h->host->SubmitFrame(img, capture_time_us, rects, &err)) return Fail(Classify(err, RDA_ERR_CODEC), err);
  return RDA_OK;
  RDA_GUARD_END
}

void rda_host_set_cursor(rda_host* h, int32_t x, int32_t y, int visible, uint64_t time_us) {
  RDA_GUARD_BEGIN
  if (h) h->host->SetCursor(x, y, visible != 0, time_us);
  RDA_GUARD_END_VOID
}

rda_status rda_host_set_cursor_image(rda_host* h, const rda_cursor_image* image) {
  RDA_GUARD_BEGIN
  if (!h || !image || !image->bgra || image->width <= 0 || image->height <= 0 || image->width > 256 ||
      image->height > 256) {
    return Fail(RDA_ERR_INVALID_ARG, "cursor image must be 1..256 pixels on each side");
  }
  rdacore::CursorImage img;
  img.width = image->width;
  img.height = image->height;
  img.hotspot_x = std::clamp(image->hotspot_x, 0, image->width - 1);
  img.hotspot_y = std::clamp(image->hotspot_y, 0, image->height - 1);
  img.bgra.assign(image->bgra, image->bgra + static_cast<size_t>(image->width) * image->height * 4);
  h->host->SetCursorImage(img);
  return RDA_OK;
  RDA_GUARD_END
}

void rda_host_request_keyframe(rda_host* h) {
  RDA_GUARD_BEGIN
  if (h) h->host->RequestKeyframe();
  RDA_GUARD_END_VOID
}

rda_state rda_host_get_state(const rda_host* h) {
  try {
    return h ? ToC(h->host->state()) : RDA_STATE_DISCONNECTED;
  } catch (...) {
    return RDA_STATE_DISCONNECTED;
  }
}

rda_status rda_host_send_message(rda_host* h, uint32_t type, const uint8_t* payload, size_t n) {
  RDA_GUARD_BEGIN
  if (!h || (!payload && n)) return Fail(RDA_ERR_INVALID_ARG, "host and payload are required");
  std::string err;
  const bool ok = h->host->SendMessage(type, std::span<const uint8_t>(payload, n), &err);
  return SendMessageStatus(ok, err);
  RDA_GUARD_END
}

rda_status rda_host_get_stats(const rda_host* h, rda_host_stats* out) {
  RDA_GUARD_BEGIN
  if (!h || !out) return Fail(RDA_ERR_INVALID_ARG, "host and out are required");
  const rdacore::HostStats s = h->host->stats();
  rda_host_stats c{};
  c.struct_size = sizeof(c);
  c.frames_captured = s.frames_captured;
  c.frames_skipped = s.frames_skipped;
  c.frames_encoded = s.frames_encoded;
  c.video_frames = s.video_frames;
  c.keyframes = s.keyframes;
  c.keyframe_requests = s.keyframe_requests;
  c.encode_errors = s.encode_errors;
  c.bytes_sent = s.bytes_sent;
  c.video_bytes = s.video_bytes;
  c.lossless_bytes = s.lossless_bytes;
  c.lossless_tiles = s.lossless_tiles;
  c.moved_tiles = s.moved_tiles;
  c.cached_tiles = s.cached_tiles;
  c.refreshed_tiles = s.refreshed_tiles;
  c.progressive_tiles = s.progressive_tiles;
  c.region_pictures = s.region_pictures;
  c.region_bytes = s.region_bytes;
  c.regions_opened = s.regions_opened;
  c.regions_closed = s.regions_closed;
  c.region_encode_us_total = s.region_encode_us_total;
  c.frames_dropped_congestion = s.frames_dropped_congestion;
  c.frames_resubmitted = s.frames_resubmitted;
  c.target_bitrate_bps = s.target_bitrate_bps;
  c.bandwidth_estimate_bps = s.bandwidth_estimate_bps;
  c.rtt_us = s.rtt_us;
  c.cursor_updates = s.cursor_updates;
  c.cursor_images = s.cursor_images;
  c.cursor_bytes = s.cursor_bytes;
  c.encode_us_total = s.encode_us_total;
  c.lossless_us_total = s.lossless_us_total;
  c.resolution_changes = s.resolution_changes;
  c.encoder_drops = s.encoder_drops;
  c.packets_sent = s.packets_sent;
  c.packets_lost = s.packets_lost;
  c.packets_spurious = s.packets_spurious;
  c.fec_parity_sent = s.fec_parity_sent;
  c.fec_recovered = s.fec_recovered;
  if (!WriteStruct(c, out)) return Fail(RDA_ERR_INVALID_ARG, "out needs struct_size");
  return RDA_OK;
  RDA_GUARD_END
}

size_t rda_host_encoder_name(const rda_host* h, char* buf, size_t buf_size) {
  try {
    return CopyName(h ? h->host->encoder_name() : std::string(), buf, buf_size);
  } catch (...) {
    return CopyName({}, buf, buf_size);
  }
}

}  // extern "C"

// ---- viewer ---------------------------------------------------------------

struct rda_viewer {
  rda_viewer_config config;
  // Callback scratch, only touched on the decode thread. Declared before
  // `viewer`, so it outlives the thread that uses it.
  std::vector<rda_rect> changed;
  CallbackGate ready;
  std::unique_ptr<rdacore::Viewer> viewer;
  ~rda_viewer() { ready.Open(); }
};

extern "C" {

void rda_viewer_config_default(rda_viewer_config* c) {
  if (!c) return;
  *c = rda_viewer_config{};
  c->struct_size = sizeof(*c);
  c->peer_timeout_ms = rdacore::ViewerOptions{}.peer_timeout_ms;
}

rda_status rda_viewer_create(rda_transport* transport, const rda_viewer_config* config, rda_viewer** out) {
  std::unique_ptr<rda_transport> owned(transport);
  RDA_GUARD_BEGIN
  if (!out) return Fail(RDA_ERR_INVALID_ARG, "out is required");
  auto v = std::make_unique<rda_viewer>();
  rda_viewer_config_default(&v->config);
  if (config && !ReadStruct(config, v->config)) return Fail(RDA_ERR_INVALID_ARG, "config needs struct_size");
  rda_viewer* raw = v.get();
  const rda_viewer_config& c = raw->config;
  if (c.peer_timeout_ms < 0) return Fail(RDA_ERR_INVALID_ARG, "peer_timeout_ms must not be negative");
  if ((!c.auth_token && c.auth_token_len) || c.auth_token_len > 4096) {
    return Fail(RDA_ERR_INVALID_ARG, "auth_token must be at most 4096 bytes");
  }
  rdacore::ViewerOptions opts;
  opts.peer_timeout_ms = c.peer_timeout_ms;
  if (auto err = TakeKey(c.encryption_key, c.allow_plaintext, owned.get(), opts.encryption_key); !err.empty()) {
    return Fail(RDA_ERR_INVALID_ARG, err);
  }
  raw->config.encryption_key = nullptr;
  if (c.auth_token_len) opts.auth_token.assign(c.auth_token, c.auth_token + c.auth_token_len);
  raw->config.auth_token = nullptr;  // not kept: the caller's memory
  raw->config.auth_token_len = 0;

  rdacore::ViewerHandlers h;
  if (c.on_stream_info) {
    h.on_stream_info = [raw](int32_t w, int32_t ht) {
      raw->ready.Wait();
      raw->config.on_stream_info(raw->config.user, w, ht);
    };
  }
  if (c.on_frame) {
    h.on_frame = [raw](const rdacore::ImageView& img, std::span<const rdacore::Rect> changed,
                       const rdacore::FrameInfo& info) {
      raw->changed.resize(changed.size());
      for (size_t i = 0; i < changed.size(); ++i) {
        raw->changed[i] = {changed[i].x, changed[i].y, changed[i].w, changed[i].h};
      }
      const rda_image ci{img.data, img.width, img.height, img.stride};
      rda_frame_info fi{};
      fi.struct_size = sizeof(fi);
      fi.frame_id = info.frame_id;
      fi.capture_time_us = info.capture_time_us;
      raw->ready.Wait();
      raw->config.on_frame(raw->config.user, &ci, raw->changed.data(), raw->changed.size(), &fi);
    };
  }
  if (c.on_cursor) {
    h.on_cursor = [raw](const rdacore::CursorState& s) {
      rda_cursor_image img{};
      rda_cursor_state cs{};
      cs.struct_size = sizeof(cs);
      cs.x = s.x;
      cs.y = s.y;
      cs.visible = s.visible;
      cs.time_us = s.time_us;
      cs.image_id = s.image_id;
      cs.image_changed = s.image_changed;
      if (s.image) {
        img = {s.image->width, s.image->height, s.image->hotspot_x, s.image->hotspot_y, s.image->bgra.data()};
        cs.image = &img;
      }
      raw->ready.Wait();
      raw->config.on_cursor(raw->config.user, &cs);
    };
  }
  if (c.on_state) {
    h.on_state = [raw](rdacore::ConnectionState st, rdacore::ConnectionError e) {
      raw->ready.Wait();
      raw->config.on_state(raw->config.user, ToC(st), ToC(e));
    };
  }
  if (c.on_message) {
    h.on_message = [raw](uint32_t type, std::span<const uint8_t> p) {
      raw->ready.Wait();
      raw->config.on_message(raw->config.user, type, p.data(), p.size());
    };
  }
  std::string err;
  std::unique_ptr<rdacore::Transport> t = owned ? std::move(owned->t) : nullptr;
  owned.reset();
  v->viewer = rdacore::Viewer::Create(std::move(t), std::move(h), std::move(opts), &err);
  if (!v->viewer) return Fail(RDA_ERR_UNSUPPORTED, err);
  *out = v.release();
  (*out)->ready.Open();
  return RDA_OK;
  RDA_GUARD_END
}

void rda_viewer_destroy(rda_viewer* v) { delete v; }

rda_status rda_viewer_send_input(rda_viewer* v, const rda_input_event* ev) {
  RDA_GUARD_BEGIN
  if (!v || !ev) return Fail(RDA_ERR_INVALID_ARG, "viewer and event are required");
  rda_input_event e;
  rda_input_event_default(&e);
  rdacore::InputEvent ie;
  if (!ReadStruct(ev, e) || !FromC(e, ie)) return Fail(RDA_ERR_INVALID_ARG, "bad input event");
  std::string err;
  if (!v->viewer->SendInput(ie, &err)) return Fail(RDA_ERR_UNSUPPORTED, err);
  return RDA_OK;
  RDA_GUARD_END
}

rda_state rda_viewer_get_state(const rda_viewer* v) {
  try {
    return v ? ToC(v->viewer->state()) : RDA_STATE_DISCONNECTED;
  } catch (...) {
    return RDA_STATE_DISCONNECTED;
  }
}

rda_status rda_viewer_send_message(rda_viewer* v, uint32_t type, const uint8_t* payload, size_t n) {
  RDA_GUARD_BEGIN
  if (!v || (!payload && n)) return Fail(RDA_ERR_INVALID_ARG, "viewer and payload are required");
  std::string err;
  const bool ok = v->viewer->SendMessage(type, std::span<const uint8_t>(payload, n), &err);
  return SendMessageStatus(ok, err);
  RDA_GUARD_END
}

void rda_viewer_request_keyframe(rda_viewer* v) {
  RDA_GUARD_BEGIN
  if (v) v->viewer->RequestKeyframe();
  RDA_GUARD_END_VOID
}

rda_status rda_viewer_feed_recording(rda_viewer* v, const uint8_t* bytes, size_t n) {
  RDA_GUARD_BEGIN
  if (!v || (!bytes && n)) return Fail(RDA_ERR_INVALID_ARG, "viewer and bytes are required");
  v->viewer->FeedRecording(std::span<const uint8_t>(bytes, n));
  return RDA_OK;
  RDA_GUARD_END
}

rda_status rda_viewer_get_stats(const rda_viewer* v, rda_viewer_stats* out) {
  RDA_GUARD_BEGIN
  if (!v || !out) return Fail(RDA_ERR_INVALID_ARG, "viewer and out are required");
  const rdacore::ViewerStats s = v->viewer->stats();
  rda_viewer_stats c{};
  c.struct_size = sizeof(c);
  c.frames_decoded = s.frames_decoded;
  c.decode_errors = s.decode_errors;
  c.bytes_received = s.bytes_received;
  c.decode_us_total = s.decode_us_total;
  c.keyframe_requests = s.keyframe_requests;
  if (!WriteStruct(c, out)) return Fail(RDA_ERR_INVALID_ARG, "out needs struct_size");
  return RDA_OK;
  RDA_GUARD_END
}

size_t rda_viewer_decoder_name(const rda_viewer* v, char* buf, size_t buf_size) {
  try {
    return CopyName(v ? v->viewer->decoder_name() : std::string(), buf, buf_size);
  } catch (...) {
    return CopyName({}, buf, buf_size);
  }
}

// ---- recordings, test pattern ---------------------------------------------

rda_status rda_recording_read(const char* path, uint8_t** bytes, size_t* n) {
  RDA_GUARD_BEGIN
  if (!path || !bytes || !n) return Fail(RDA_ERR_INVALID_ARG, "path, bytes and n are required");
  *bytes = nullptr;
  *n = 0;
  std::vector<uint8_t> data;
  std::string err;
  if (!rdacore::ReadRecording(path, data, &err)) return Fail(RDA_ERR_IO, err);
  auto* p = static_cast<uint8_t*>(std::malloc(std::max<size_t>(1, data.size())));
  if (!p) return Fail(RDA_ERR_NO_MEMORY, "out of memory");
  if (!data.empty()) std::memcpy(p, data.data(), data.size());
  *bytes = p;
  *n = data.size();
  return RDA_OK;
  RDA_GUARD_END
}

void rda_free(void* p) { std::free(p); }

uint32_t rda_keycode_from_mac(uint32_t mac_keycode) { return rdacore::HidFromMacKeyCode(mac_keycode); }
uint32_t rda_keycode_to_mac(uint32_t hid_usage) { return rdacore::HidToMacKeyCode(hid_usage); }
uint32_t rda_keycode_from_evdev(uint32_t evdev_code) { return rdacore::HidFromEvdevKeyCode(evdev_code); }
uint32_t rda_keycode_to_evdev(uint32_t hid_usage) { return rdacore::HidToEvdevKeyCode(hid_usage); }

rda_status rda_test_pattern_render(uint64_t index, int32_t width, int32_t height, uint8_t* bgra,
                                   int incremental, rda_rect* rects, size_t max_rects, size_t* rect_count) {
  RDA_GUARD_BEGIN
  if (!bgra || width < 64 || height < 64 || width > 16384 || height > 16384 || (!rects && max_rects)) {
    return Fail(RDA_ERR_INVALID_ARG, "bgra is required and the size must be 64..16384");
  }
  const auto dirty = rdacore::capture::RenderSyntheticFrame(index, width, height, bgra, incremental == 0);
  for (size_t i = 0; i < dirty.size() && i < max_rects; ++i) {
    rects[i] = {dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h};
  }
  if (rect_count) *rect_count = dirty.size();
  return RDA_OK;
  RDA_GUARD_END
}

}  // extern "C"
