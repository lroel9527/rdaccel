#include "linux/capture_pipewire.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

extern "C" {
#include <libswscale/swscale.h>
}

#include "log/log.h"

namespace rdacore::platform {

// ---------------------------------------------------------------------------
// Pixel formats
// ---------------------------------------------------------------------------

namespace pw_detail {

bool ChannelOrder(uint32_t f, int& b, int& g, int& r) {
  switch (f) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
      b = 0, g = 1, r = 2;
      return true;
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGBA:
      b = 2, g = 1, r = 0;
      return true;
    case SPA_VIDEO_FORMAT_xRGB:
    case SPA_VIDEO_FORMAT_ARGB:
      b = 3, g = 2, r = 1;
      return true;
    case SPA_VIDEO_FORMAT_xBGR:
    case SPA_VIDEO_FORMAT_ABGR:
      b = 1, g = 2, r = 3;
      return true;
    default:
      return false;
  }
}

void ToBgra(const uint8_t* src, int32_t src_stride, int32_t w, int32_t h, int b, int g, int r,
            uint8_t* dst, int32_t dst_stride) {
  for (int32_t y = 0; y < h; ++y) {
    const uint8_t* s = src + static_cast<ptrdiff_t>(y) * src_stride;
    uint8_t* d = dst + static_cast<ptrdiff_t>(y) * dst_stride;
    for (int32_t x = 0; x < w; ++x, s += 4, d += 4) {
      d[0] = s[b];
      d[1] = s[g];
      d[2] = s[r];
      d[3] = 255;
    }
  }
}

}  // namespace pw_detail

namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Process-wide state
// ---------------------------------------------------------------------------

std::mutex g_mu;
std::string g_restore_token;  // remembered for the life of the process
int32_t g_last_w = 0, g_last_h = 0;
std::weak_ptr<PipeWireCursor> g_cursor;  // of the running capture, if any
bool g_cursor_embedded = false;          // the running capture draws it into the picture

std::string TokenFile() {
  const char* f = std::getenv("RDACCEL_PORTAL_TOKEN_FILE");
  return f && *f ? f : "";
}

std::string LoadRestoreToken() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_restore_token.empty()) {
    if (const std::string path = TokenFile(); !path.empty()) {
      std::ifstream in(path);
      std::getline(in, g_restore_token);
    }
  }
  return g_restore_token;
}

void SaveRestoreToken(const std::string& token) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_restore_token = token;
  if (const std::string path = TokenFile(); !path.empty() && !token.empty()) {
    std::ofstream out(path, std::ios::trunc);
    out << token << "\n";
    if (!out) log::Warn("pipewire: cannot write the portal restore token to " + path);
  }
}

}  // namespace

struct PipeWireCursor {
  std::mutex mu;
  bool have = false;  // a position arrived
  double x = 0, y = 0;
  int32_t frame_w = 0, frame_h = 0;
  bool visible = false;
  uint64_t serial = 0;  // bumped with each new image
  CursorImage image;
};

namespace {

// ---------------------------------------------------------------------------
// xdg-desktop-portal ScreenCast over D-Bus
// ---------------------------------------------------------------------------

constexpr const char* kPortalBus = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";

// ScreenCast source types and cursor modes (bit flags).
constexpr uint32_t kSourceMonitor = 1;
constexpr uint32_t kCursorHidden = 1, kCursorEmbedded = 2, kCursorMetadata = 4;

struct GErrorFree {
  void operator()(GError* e) const { g_error_free(e); }
};
using GErrorPtr = std::unique_ptr<GError, GErrorFree>;
struct GVariantUnref {
  void operator()(GVariant* v) const { g_variant_unref(v); }
};
using GVariantPtr = std::unique_ptr<GVariant, GVariantUnref>;

std::string Message(GError* e) { return e && e->message ? e->message : "unknown error"; }

class Portal {
 public:
  ~Portal() { Close(); }

  bool Connect(std::string* error) {
    ctx_ = g_main_context_new();
    g_main_context_push_thread_default(ctx_);
    GError* e = nullptr;
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &e);
    if (address) {
      // A private connection: the portal ends the session when it closes.
      conn_ = g_dbus_connection_new_for_address_sync(
          address,
          static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
          nullptr, nullptr, &e);
      g_free(address);
    }
    g_main_context_pop_thread_default(ctx_);
    if (!conn_) {
      *error = "no D-Bus session bus: " + Message(e);
      if (e) g_error_free(e);
      return false;
    }
    std::string sender = g_dbus_connection_get_unique_name(conn_);
    if (!sender.empty() && sender[0] == ':') sender.erase(0, 1);
    std::replace(sender.begin(), sender.end(), '.', '_');
    sender_ = sender;
    // Unique within the process and across processes on this bus.
    static std::atomic<uint32_t> instances{0};
    std::random_device rd;
    token_base_ = "rdaccel" + std::to_string(getpid()) + "_" + std::to_string(++instances) + "_" +
                  std::to_string(rd() & 0xffff);
    return true;
  }

  // A uint32 property of the ScreenCast interface, or 0.
  uint32_t Property(const char* name) {
    GError* e = nullptr;
    GVariantPtr r(g_dbus_connection_call_sync(conn_, kPortalBus, kPortalPath, "org.freedesktop.DBus.Properties",
                                              "Get", g_variant_new("(ss)", kScreenCast, name),
                                              G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &e));
    if (!r) {
      if (e) g_error_free(e);
      return 0;
    }
    GVariant* v = nullptr;
    g_variant_get(r.get(), "(v)", &v);
    GVariantPtr holder(v);
    return g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v) : 0;
  }

  std::string NewToken() { return token_base_ + "_" + std::to_string(++counter_); }

  // Calls `method` (whose last argument is an options a{sv} that carries
  // handle_token = `token`) and waits for its Request.Response signal.
  // `args` is consumed. On success `results` holds the response's a{sv}.
  bool Request(const char* method, GVariant* args, const std::string& token, int timeout_ms,
               GVariantPtr& results, std::string* error) {
    const std::string path = std::string(kPortalPath) + "/request/" + sender_ + "/" + token;
    struct Response {
      bool done = false;
      uint32_t code = 2;
      GVariant* results = nullptr;
    } resp;
    g_main_context_push_thread_default(ctx_);
    const guint sub = g_dbus_connection_signal_subscribe(
        conn_, kPortalBus, "org.freedesktop.portal.Request", "Response", path.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params,
           gpointer user) {
          auto* r = static_cast<Response*>(user);
          if (r->done) return;
          g_variant_get(params, "(u@a{sv})", &r->code, &r->results);
          r->done = true;
        },
        &resp, nullptr);
    GError* e = nullptr;
    GVariantPtr handle(g_dbus_connection_call_sync(conn_, kPortalBus, kPortalPath, kScreenCast, method, args,
                                                   G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 10000,
                                                   nullptr, &e));
    bool ok = handle != nullptr;
    if (!ok) {
      *error = std::string("portal ScreenCast.") + method + " failed: " + Message(e);
      if (e) g_error_free(e);
    } else {
      // Wait for the answer (Start may wait for the user to pick a screen).
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
      GSource* tick = g_timeout_source_new(50);
      g_source_set_callback(tick, [](gpointer) -> gboolean { return G_SOURCE_CONTINUE; }, nullptr, nullptr);
      g_source_attach(tick, ctx_);
      while (!resp.done && std::chrono::steady_clock::now() < deadline) g_main_context_iteration(ctx_, TRUE);
      g_source_destroy(tick);
      g_source_unref(tick);
      if (!resp.done) {
        *error = std::string("portal ScreenCast.") + method + ": no answer (timed out)";
        ok = false;
      } else if (resp.code != 0) {
        *error = std::string("portal ScreenCast.") + method +
                 (resp.code == 1 ? ": the user cancelled screen sharing" : ": refused by the portal");
        ok = false;
      }
    }
    g_dbus_connection_signal_unsubscribe(conn_, sub);
    g_main_context_pop_thread_default(ctx_);
    if (resp.results) {
      if (ok) {
        results.reset(resp.results);
      } else {
        g_variant_unref(resp.results);
      }
    }
    return ok;
  }

  bool Handshake(const capture::CaptureConfig& cfg, uint32_t& node, int& fd, int32_t& w, int32_t& h,
                 bool& cursor_metadata, bool& cursor_embedded, std::string* error) {
    const uint32_t version = Property("version");
    if (version == 0) {
      *error = "no screen-cast portal (is xdg-desktop-portal running, with a backend for this desktop?)";
      return false;
    }
    const uint32_t cursor_modes = Property("AvailableCursorModes");
    const uint32_t source_types = Property("AvailableSourceTypes");
    if (source_types && !(source_types & kSourceMonitor)) {
      *error = "the screen-cast portal cannot share monitors";
      return false;
    }
    const int timeout_ms = [] {
      const char* t = std::getenv("RDACCEL_PORTAL_TIMEOUT_S");
      const int s = t ? std::atoi(t) : 0;
      return (s > 0 ? s : 120) * 1000;
    }();

    // CreateSession
    GVariantPtr res;
    std::string token = NewToken();
    {
      GVariantBuilder o;
      g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&o, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
      g_variant_builder_add(&o, "{sv}", "session_handle_token", g_variant_new_string(NewToken().c_str()));
      if (!Request("CreateSession", g_variant_new("(a{sv})", &o), token, 10000, res, error)) return false;
      const gchar* s = nullptr;
      if (!g_variant_lookup(res.get(), "session_handle", "&s", &s) || !s) {
        *error = "portal CreateSession: no session handle";
        return false;
      }
      session_ = s;
    }

    // SelectSources: one monitor; the cursor as metadata when possible, so
    // that it can travel on the cursor channel, else drawn into the picture.
    if (cfg.show_cursor && (cursor_modes & kCursorEmbedded)) {
      cursor_mode_ = kCursorEmbedded;
    } else if (cursor_modes & kCursorMetadata) {
      cursor_mode_ = kCursorMetadata;
    } else if (cursor_modes & kCursorEmbedded) {
      cursor_mode_ = kCursorEmbedded;
    } else {
      cursor_mode_ = kCursorHidden;
    }
    cursor_metadata = cursor_mode_ == kCursorMetadata;
    cursor_embedded = cursor_mode_ == kCursorEmbedded;
    token = NewToken();
    {
      GVariantBuilder o;
      g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&o, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
      g_variant_builder_add(&o, "{sv}", "types", g_variant_new_uint32(kSourceMonitor));
      g_variant_builder_add(&o, "{sv}", "multiple", g_variant_new_boolean(FALSE));
      if (cursor_modes) g_variant_builder_add(&o, "{sv}", "cursor_mode", g_variant_new_uint32(cursor_mode_));
      if (version >= 4) {
        // 1 = for the life of this process; 2 = until revoked (with a file
        // to keep the token in).
        g_variant_builder_add(&o, "{sv}", "persist_mode", g_variant_new_uint32(TokenFile().empty() ? 1 : 2));
        const std::string restore = LoadRestoreToken();
        if (!restore.empty()) {
          g_variant_builder_add(&o, "{sv}", "restore_token", g_variant_new_string(restore.c_str()));
        }
      }
      if (!Request("SelectSources", g_variant_new("(oa{sv})", session_.c_str(), &o), token, 10000, res, error)) {
        return false;
      }
    }

    // Start: this is where a desktop asks the user.
    token = NewToken();
    {
      GVariantBuilder o;
      g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&o, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
      if (!Request("Start", g_variant_new("(osa{sv})", session_.c_str(), "", &o), token, timeout_ms, res,
                   error)) {
        return false;
      }
      const gchar* restore = nullptr;
      if (g_variant_lookup(res.get(), "restore_token", "&s", &restore) && restore) SaveRestoreToken(restore);
      GVariant* streams = g_variant_lookup_value(res.get(), "streams", G_VARIANT_TYPE("a(ua{sv})"));
      if (!streams || g_variant_n_children(streams) == 0) {
        if (streams) g_variant_unref(streams);
        *error = "portal Start: no stream was shared";
        return false;
      }
      GVariant* props = nullptr;
      g_variant_get_child(streams, 0, "(u@a{sv})", &node, &props);
      w = h = 0;
      g_variant_lookup(props, "size", "(ii)", &w, &h);
      g_variant_unref(props);
      g_variant_unref(streams);
    }

    // OpenPipeWireRemote: a PipeWire connection that may read this stream.
    GError* e = nullptr;
    GUnixFDList* fds = nullptr;
    GVariantBuilder o;
    g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
    GVariantPtr r(g_dbus_connection_call_with_unix_fd_list_sync(
        conn_, kPortalBus, kPortalPath, kScreenCast, "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", session_.c_str(), &o), G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 10000,
        nullptr, &fds, nullptr, &e));
    if (!r) {
      *error = "portal OpenPipeWireRemote failed: " + Message(e);
      if (e) g_error_free(e);
      return false;
    }
    gint32 index = -1;
    g_variant_get(r.get(), "(h)", &index);
    fd = fds ? g_unix_fd_list_get(fds, index, &e) : -1;
    if (fds) g_object_unref(fds);
    if (fd < 0) {
      *error = "portal OpenPipeWireRemote: no file descriptor: " + Message(e);
      if (e) g_error_free(e);
      return false;
    }
    return true;
  }

  void Close() {
    if (conn_ && !session_.empty()) {
      GError* e = nullptr;
      GVariant* r = g_dbus_connection_call_sync(conn_, kPortalBus, session_.c_str(),
                                                "org.freedesktop.portal.Session", "Close", nullptr, nullptr,
                                                G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &e);
      if (r) g_variant_unref(r);
      if (e) g_error_free(e);
    }
    session_.clear();
    if (conn_) {
      g_dbus_connection_close_sync(conn_, nullptr, nullptr);
      g_object_unref(conn_);
      conn_ = nullptr;
    }
    if (ctx_) {
      // Let pending callbacks (with references to the connection) finish.
      while (g_main_context_iteration(ctx_, FALSE)) {
      }
      g_main_context_unref(ctx_);
      ctx_ = nullptr;
    }
  }

 private:
  GMainContext* ctx_ = nullptr;
  GDBusConnection* conn_ = nullptr;
  std::string sender_, token_base_, session_;
  uint32_t counter_ = 0;
  uint32_t cursor_mode_ = kCursorHidden;
};

// ---------------------------------------------------------------------------
// The PipeWire video stream
// ---------------------------------------------------------------------------

struct SwsFree {
  void operator()(SwsContext* s) const { sws_freeContext(s); }
};

}  // namespace

struct PipeWireCapturer::Impl {
  Portal portal;
  pw_thread_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_stream* stream = nullptr;
  spa_hook stream_listener{};
  spa_hook core_listener{};

  capture::CaptureConfig cfg;
  capture::FrameFn on_frame;
  capture::StoppedFn on_stopped;
  bool running = false;          // Start succeeded; under the loop lock
  bool stopped_reported = false;
  std::shared_ptr<PipeWireCursor> cursor;  // with cursor metadata only

  // Negotiated format; guarded by the loop lock like everything below.
  bool have_format = false;
  uint32_t format = 0;
  int32_t width = 0, height = 0;
  int ob = 0, og = 0, orr = 0;  // channel order
  pw_stream_state state = PW_STREAM_STATE_UNCONNECTED;
  std::string stream_error;

  // Damage of frames not yet delivered (dropped or empty buffers).
  std::vector<Rect> pending;
  bool pending_full = true;

  std::vector<uint8_t> converted, scaled;
  std::unique_ptr<SwsContext, SwsFree> sws;
  int32_t sws_w = 0, sws_h = 0, sws_ow = 0, sws_oh = 0;
  uint64_t frames = 0;

  ~Impl() { Teardown(); }

  void Teardown() {
    // End the portal session first, so the compositor stops its stream
    // while ours still runs.
    portal.Close();
    if (loop) pw_thread_loop_stop(loop);
    if (stream) {
      spa_hook_remove(&stream_listener);
      pw_stream_destroy(stream);
      stream = nullptr;
    }
    if (core) {
      spa_hook_remove(&core_listener);
      pw_core_disconnect(core);
      core = nullptr;
    }
    if (context) {
      pw_context_destroy(context);
      context = nullptr;
    }
    if (loop) {
      pw_thread_loop_destroy(loop);
      loop = nullptr;
    }
    portal.Close();
  }

  static void OnStateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error) {
    auto* self = static_cast<Impl*>(data);
    self->state = state;
    std::string ended;
    if (state == PW_STREAM_STATE_ERROR) {
      self->stream_error = error ? error : "stream error";
      log::Warn("pipewire: " + self->stream_error);
      ended = "PipeWire capture failed: " + self->stream_error;
    } else if (state == PW_STREAM_STATE_UNCONNECTED && self->frames > 0) {
      log::Warn("pipewire: the screen-cast stream ended (sharing stopped?)");
      ended = "the screen-cast stream ended (sharing stopped?)";
    }
    if (!ended.empty() && self->running && !self->stopped_reported) {
      self->stopped_reported = true;
      if (self->on_stopped) self->on_stopped(ended);
    }
    pw_thread_loop_signal(self->loop, false);
  }

  static void OnParamChanged(void* data, uint32_t id, const spa_pod* param) {
    auto* self = static_cast<Impl*>(data);
    if (id != SPA_PARAM_Format || !param) return;
    uint32_t media_type = 0, media_subtype = 0;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0 || media_type != SPA_MEDIA_TYPE_video ||
        media_subtype != SPA_MEDIA_SUBTYPE_raw) {
      return;
    }
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    int b = 0, g = 0, r = 0;
    if (!pw_detail::ChannelOrder(info.format, b, g, r)) {
      self->stream_error = "unsupported pixel format " + std::to_string(info.format);
      self->have_format = false;
      return;
    }
    self->format = info.format;
    self->width = static_cast<int32_t>(info.size.width);
    self->height = static_cast<int32_t>(info.size.height);
    self->ob = b, self->og = g, self->orr = r;
    self->have_format = true;
    self->pending_full = true;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      g_last_w = self->width;
      g_last_h = self->height;
    }
    log::Info("pipewire: " + std::to_string(self->width) + "x" + std::to_string(self->height) + " format " +
              std::to_string(info.format));

    // Buffers in ordinary memory (no DMA-BUF: no modifiers were offered),
    // plus the metadata we use.
    uint8_t buf[1024];
    spa_pod_builder b2 = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const spa_pod* params[5];
    uint32_t n = 0;
    params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b2, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
    params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b2, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))));
    params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b2, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region))));
    params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b2, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(sizeof(spa_meta_region) * 16, sizeof(spa_meta_region) * 1,
                                 sizeof(spa_meta_region) * 16)));
    if (self->cursor) {
      constexpr int32_t kCursorBytes = sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + 256 * 256 * 4;
      constexpr int32_t kCursorMin = sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + 1 * 1 * 4;
      params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
          &b2, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
          SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(kCursorBytes, kCursorMin, kCursorBytes)));
    }
    pw_stream_update_params(self->stream, params, n);
  }

  // Adds the damage of `buf` (in frame coordinates, cropped by `crop`) to
  // the pending damage; no damage metadata = everything.
  void AddDamage(spa_buffer* buf, const Rect& crop) {
    spa_meta* m = spa_buffer_find_meta(buf, SPA_META_VideoDamage);
    if (!m) {
      pending_full = true;
      return;
    }
    bool any = false;
    spa_meta_region* r = nullptr;
    spa_meta_for_each(r, m) {
      if (!spa_meta_region_is_valid(r)) break;
      Rect d{r->region.position.x - crop.x, r->region.position.y - crop.y,
             static_cast<int32_t>(r->region.size.width), static_cast<int32_t>(r->region.size.height)};
      const int32_t x0 = std::max(0, d.x), y0 = std::max(0, d.y);
      const int32_t x1 = std::min(crop.w, d.x + d.w), y1 = std::min(crop.h, d.y + d.h);
      if (x1 > x0 && y1 > y0) pending.push_back({x0, y0, x1 - x0, y1 - y0});
      any = true;
    }
    // A buffer with an empty damage list carries a new picture all the same.
    if (!any) pending_full = true;
    if (pending.size() > 64) pending_full = true;
  }

  void ReadCursor(spa_buffer* buf) {
    if (!cursor) return;
    auto* c = static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(spa_meta_cursor)));
    if (!c || !spa_meta_cursor_is_valid(c)) return;
    std::lock_guard<std::mutex> lock(cursor->mu);
    cursor->have = true;
    cursor->x = c->position.x;
    cursor->y = c->position.y;
    cursor->frame_w = width;
    cursor->frame_h = height;
    if (c->bitmap_offset < sizeof(spa_meta_cursor)) return;  // no new image
    auto* bm = SPA_PTROFF(c, c->bitmap_offset, spa_meta_bitmap);
    if (!spa_meta_bitmap_is_valid(bm)) return;
    if (bm->offset == 0 || bm->size.width == 0 || bm->size.height == 0) {
      cursor->visible = false;  // no image = invisible
      return;
    }
    cursor->visible = true;
    int b = 0, g = 0, r = 0;
    if (!pw_detail::ChannelOrder(bm->format, b, g, r)) return;
    const auto w = static_cast<int32_t>(std::min<uint32_t>(bm->size.width, 256));
    const auto h = static_cast<int32_t>(std::min<uint32_t>(bm->size.height, 256));
    const auto* src = SPA_PTROFF(bm, bm->offset, const uint8_t);
    const int32_t stride = bm->stride > 0 ? bm->stride : static_cast<int32_t>(bm->size.width) * 4;
    CursorImage img;
    img.width = w;
    img.height = h;
    img.hotspot_x = std::clamp<int32_t>(c->hotspot.x, 0, w - 1);
    img.hotspot_y = std::clamp<int32_t>(c->hotspot.y, 0, h - 1);
    img.bgra.resize(static_cast<size_t>(w) * h * 4);
    // Cursor bitmaps are premultiplied (as compositors draw them), which is
    // what CursorImage holds; keep the alpha channel.
    for (int32_t y = 0; y < h; ++y) {
      const uint8_t* s = src + static_cast<ptrdiff_t>(y) * stride;
      uint8_t* d = &img.bgra[static_cast<size_t>(y) * w * 4];
      for (int32_t x = 0; x < w; ++x, s += 4, d += 4) {
        d[0] = s[b];
        d[1] = s[g];
        d[2] = s[r];
        d[3] = s[6 - b - g - r];  // the remaining byte is alpha
      }
    }
    if (!(img == cursor->image)) {
      cursor->image = std::move(img);
      ++cursor->serial;
    }
  }

  static void OnProcess(void* data) {
    auto* self = static_cast<Impl*>(data);
    // Only the newest picture matters; older ones only add their damage.
    pw_buffer* newest = nullptr;
    while (pw_buffer* b = pw_stream_dequeue_buffer(self->stream)) {
      if (newest) {
        self->Consume(newest, false);
        pw_stream_queue_buffer(self->stream, newest);
      }
      newest = b;
    }
    if (!newest) return;
    self->Consume(newest, true);
    pw_stream_queue_buffer(self->stream, newest);
  }

  // Handles one buffer: the cursor and damage always, the picture if
  // `deliver`.
  void Consume(pw_buffer* pb, bool deliver) {
    spa_buffer* buf = pb->buffer;
    ReadCursor(buf);
    if (!have_format || buf->n_datas < 1) return;
    const spa_data& d = buf->datas[0];
    auto* hdr = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header)));
    if (hdr && (hdr->flags & SPA_META_HEADER_FLAG_CORRUPTED)) return;
    // Cursor-only updates come without a picture.
    if (!d.data || !d.chunk || d.chunk->size == 0 || (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) return;

    Rect crop{0, 0, width, height};
    if (auto* c = static_cast<spa_meta_region*>(spa_buffer_find_meta_data(buf, SPA_META_VideoCrop, sizeof(spa_meta_region)));
        c && spa_meta_region_is_valid(c)) {
      Rect r{c->region.position.x, c->region.position.y, static_cast<int32_t>(c->region.size.width),
             static_cast<int32_t>(c->region.size.height)};
      if (r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= width && r.y + r.h <= height) crop = r;
    }
    AddDamage(buf, crop);
    if (!deliver) return;

    const int32_t stride = d.chunk->stride != 0 ? d.chunk->stride : width * 4;
    const size_t needed = static_cast<size_t>(d.chunk->offset) + static_cast<size_t>(std::abs(stride)) * height;
    if (stride < width * 4 || needed > d.maxsize) {
      pending_full = true;
      return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(d.data) + d.chunk->offset +
                          static_cast<ptrdiff_t>(crop.y) * stride + static_cast<ptrdiff_t>(crop.x) * 4;
    const int32_t w = crop.w & ~1, h = crop.h & ~1;  // even sizes, as the encoder needs
    if (w < 2 || h < 2) return;

    capture::CapturedFrame f;
    if (ob == 0 && og == 1 && orr == 2) {
      f.bgra = {base, w, h, stride};
    } else {
      converted.resize(static_cast<size_t>(w) * h * 4);
      pw_detail::ToBgra(base, stride, w, h, ob, og, orr, converted.data(), w * 4);
      f.bgra = {converted.data(), w, h, w * 4};
    }
    int32_t out_w = w, out_h = h;
    if (cfg.width > 0 && cfg.height > 0 && (cfg.width != w || cfg.height != h)) {
      out_w = cfg.width & ~1;
      out_h = cfg.height & ~1;
    }
    bool full = pending_full;
    if (out_w != w || out_h != h) {
      if (!sws || sws_w != w || sws_h != h || sws_ow != out_w || sws_oh != out_h) {
        sws.reset(sws_getContext(w, h, AV_PIX_FMT_BGRA, out_w, out_h, AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr,
                                 nullptr));
        sws_w = w, sws_h = h, sws_ow = out_w, sws_oh = out_h;
      }
      if (!sws) return;
      scaled.resize(static_cast<size_t>(out_w) * out_h * 4);
      const uint8_t* src[1] = {f.bgra.data};
      const int src_stride[1] = {f.bgra.stride};
      uint8_t* dst[1] = {scaled.data()};
      const int dst_stride[1] = {out_w * 4};
      sws_scale(sws.get(), src, src_stride, 0, h, dst, dst_stride);
      f.bgra = {scaled.data(), out_w, out_h, out_w * 4};
      full = true;  // damage is not scaled
    }
    if (!full) {
      for (Rect& r : pending) {
        r.w = std::min(r.w, w - r.x);
        r.h = std::min(r.h, h - r.y);
        if (r.w > 0 && r.h > 0) f.dirty.push_back(r);
      }
      // Damage that is all outside the (even-sized) picture: nothing changed
      // that we show, but deliver the whole frame rather than guess.
    }
    pending.clear();
    pending_full = false;
    f.capture_time_us = capture::MonotonicMicros();
    ++frames;
    on_frame(f);
  }

  bool Start(int fd, uint32_t node, std::string* error) {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
    loop = pw_thread_loop_new("rdaccel-pipewire", nullptr);
    if (!loop) {
      *error = "pw_thread_loop_new failed";
      if (fd >= 0) close(fd);
      return false;
    }
    context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    if (!context || pw_thread_loop_start(loop) < 0) {
      *error = "cannot start the PipeWire loop";
      if (fd >= 0) close(fd);
      return false;
    }
    pw_thread_loop_lock(loop);
    core = fd >= 0 ? pw_context_connect_fd(context, fd, nullptr, 0) : pw_context_connect(context, nullptr, 0);
    if (!core) {
      pw_thread_loop_unlock(loop);
      *error = std::string("cannot connect to PipeWire: ") + strerror(errno);
      return false;
    }
    static const pw_core_events core_events = [] {
      pw_core_events e{};
      e.version = PW_VERSION_CORE_EVENTS;
      e.error = [](void* data, uint32_t id, int, int res, const char* message) {
        auto* self = static_cast<Impl*>(data);
        if (id != PW_ID_CORE) return;
        self->stream_error = std::string("PipeWire: ") + (message ? message : spa_strerror(res));
        log::Warn(self->stream_error);
        pw_thread_loop_signal(self->loop, false);
      };
      return e;
    }();
    pw_core_add_listener(core, &core_listener, &core_events, this);

    // Only this node (passed to pw_stream_connect below; target.object would
    // want its serial): no fallback to another video source.
    stream = pw_stream_new(core, "rdaccel",
                           pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_MEDIA_ROLE, "Screen",
                                             "node.dont-fallback", "true", "node.dont-reconnect", "true",
                                             nullptr));
    if (!stream) {
      pw_thread_loop_unlock(loop);
      *error = "pw_stream_new failed";
      return false;
    }
    static const pw_stream_events stream_events = [] {
      pw_stream_events e{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.state_changed = &Impl::OnStateChanged;
      e.param_changed = &Impl::OnParamChanged;
      e.process = &Impl::OnProcess;
      return e;
    }();
    pw_stream_add_listener(stream, &stream_listener, &stream_events, this);

    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    spa_rectangle def_size{1920, 1080}, min_size{1, 1}, max_size{16384, 16384};
    const int fps = cfg.fps > 0 ? cfg.fps : 60;
    spa_fraction def_rate{static_cast<uint32_t>(fps), 1}, min_rate{0, 1}, max_rate{360, 1};
    const spa_pod* params[1];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(9, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_xRGB,
                               SPA_VIDEO_FORMAT_ARGB, SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_ABGR),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&def_rate, &min_rate, &max_rate)));
    // Wait until the stream runs (or fails). The session manager may not
    // know a node the portal has just made yet ("target not found"): try
    // again for a while.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    bool ok = false;
    for (int attempt = 0;; ++attempt) {
      state = PW_STREAM_STATE_UNCONNECTED;
      stream_error.clear();
      const int rc = pw_stream_connect(
          stream, PW_DIRECTION_INPUT, node,
          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
      if (rc < 0) {
        stream_error = std::string("pw_stream_connect failed: ") + spa_strerror(rc);
        break;
      }
      while (state != PW_STREAM_STATE_STREAMING && state != PW_STREAM_STATE_ERROR && stream_error.empty() &&
             std::chrono::steady_clock::now() < deadline) {
        pw_thread_loop_timed_wait(loop, 1);
      }
      ok = state == PW_STREAM_STATE_STREAMING && stream_error.empty();
      if (ok || std::chrono::steady_clock::now() >= deadline ||
          stream_error.find("target not found") == std::string::npos || attempt >= 20) {
        break;
      }
      pw_stream_disconnect(stream);
      pw_thread_loop_unlock(loop);
      std::this_thread::sleep_for(100ms);
      pw_thread_loop_lock(loop);
    }
    if (!ok) {
      *error = !stream_error.empty() ? stream_error : "the PipeWire stream did not start (no such node?)";
    }
    pw_thread_loop_unlock(loop);
    return ok;
  }
};

PipeWireCapturer::PipeWireCapturer() = default;
PipeWireCapturer::PipeWireCapturer(uint32_t node_id) : node_(node_id) {}
PipeWireCapturer::~PipeWireCapturer() { Stop(); }

bool PipeWireCapturer::Start(const capture::CaptureConfig& cfg, capture::FrameFn on_frame) {
  if (impl_) {
    error_ = "already running";
    return false;
  }
  if (cfg.display_index != 0) {
    error_ = "on Wayland the shared display is picked in the screen-sharing dialog; display_index must be 0";
    return false;
  }
  auto impl = std::make_unique<Impl>();
  impl->cfg = cfg;
  impl->on_frame = std::move(on_frame);
  impl->on_stopped = on_stopped_;
  int fd = -1;
  uint32_t node = 0;
  bool cursor_metadata = false, cursor_embedded = false;
  if (node_) {
    node = *node_;
    cursor_metadata = !cfg.show_cursor;
  } else {
    int32_t w = 0, h = 0;
    if (!impl->portal.Connect(&error_) || !impl->portal.Handshake(cfg, node, fd, w, h, cursor_metadata, cursor_embedded,
                                                                &error_)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (w > 0 && h > 0) g_last_w = w, g_last_h = h;
  }
  if (cursor_metadata) impl->cursor = std::make_shared<PipeWireCursor>();
  if (!impl->Start(fd, node, &error_)) return false;
  pw_thread_loop_lock(impl->loop);
  impl->running = true;
  pw_thread_loop_unlock(impl->loop);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_cursor = impl->cursor;
    g_cursor_embedded = cursor_embedded && !cfg.show_cursor;
  }
  impl_ = std::move(impl);
  return true;
}

void PipeWireCapturer::Stop() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_cursor.lock() == impl_->cursor) {
      g_cursor.reset();
      g_cursor_embedded = false;
    }
  }
  pw_thread_loop_lock(impl_->loop);
  impl_->running = false;  // our own teardown is not "stopped by itself"
  pw_thread_loop_unlock(impl_->loop);
  impl_->Teardown();
  impl_.reset();
}

bool UseWaylandCapture() {
  if (const char* v = std::getenv("RDACCEL_CAPTURE"); v && *v) {
    if (std::strcmp(v, "x11") == 0) return false;
    if (std::strcmp(v, "pipewire") == 0 || std::strcmp(v, "portal") == 0) return true;
  }
  if (const char* t = std::getenv("XDG_SESSION_TYPE"); t && std::strcmp(t, "wayland") == 0) return true;
  const char* wl = std::getenv("WAYLAND_DISPLAY");
  const char* x = std::getenv("DISPLAY");
  return wl && *wl && !(x && *x);
}

void LastPipeWireSize(int32_t& width, int32_t& height) {
  std::lock_guard<std::mutex> lock(g_mu);
  width = g_last_w;
  height = g_last_h;
}

namespace {

class PipeWireCursorSource final : public capture::ICursorSource {
 public:
  explicit PipeWireCursorSource(std::shared_ptr<PipeWireCursor> c) : c_(std::move(c)) {}
  bool Poll(capture::CursorSample& out) override {
    std::lock_guard<std::mutex> lock(c_->mu);
    if (!c_->have || c_->frame_w <= 0) return false;
    out.x = c_->x;
    out.y = c_->y;
    out.screen_width = c_->frame_w;
    out.screen_height = c_->frame_h;
    out.image_scale = 1;
    out.visible = c_->visible && out.x >= 0 && out.y >= 0 && out.x < c_->frame_w && out.y < c_->frame_h;
    out.image_changed = c_->serial != serial_ && c_->image.width > 0;
    if (out.image_changed) {
      out.image = c_->image;
      serial_ = c_->serial;
    }
    return true;
  }

 private:
  std::shared_ptr<PipeWireCursor> c_;
  uint64_t serial_ = 0;
};

}  // namespace

std::unique_ptr<capture::ICursorSource> CreatePipeWireCursorSource(std::string* error) {
  std::shared_ptr<PipeWireCursor> c;
  bool embedded = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    c = g_cursor.lock();
    embedded = g_cursor_embedded;
  }
  if (!c) {
    if (error) {
      // Empty when nothing is missing: the viewer sees the cursor in the picture.
      *error = embedded ? "" : "no Wayland screen capture with cursor metadata is running";
    }
    return nullptr;
  }
  return std::make_unique<PipeWireCursorSource>(std::move(c));
}

}  // namespace rdacore::platform
