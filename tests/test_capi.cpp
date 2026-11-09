// Tests of the C ABI itself (include/rdaccel/rdaccel.h): argument checking,
// status codes, versioned structs, custom transports through the vtable, and
// the log sink. Uses only the C header.

#include <rdaccel/rdaccel.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int32_t kW = 640, kH = 360;

bool WaitUntil(const std::function<bool()>& done, int ms = 10000) {
  for (int i = 0; i < ms / 5; ++i) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

struct Counter {
  std::atomic<int> frames{0};
  std::atomic<int> inputs{0};
  std::atomic<uint32_t> last_code{0};
};

}  // namespace

TEST(CApi, Version) {
  EXPECT_EQ(rda_version(), RDA_VERSION);
  EXPECT_EQ(rda_version(), RDA_MAKE_VERSION(1, 1, 0));
  EXPECT_STREQ(rda_version_string(), "1.1.0");
  EXPECT_GT(std::strlen(rda_platform_name()), 0u);
  EXPECT_STREQ(rda_status_str(RDA_ERR_TIMEOUT), "RDA_ERR_TIMEOUT");
  EXPECT_STREQ(rda_status_str(static_cast<rda_status>(-999)), "RDA_ERR_UNKNOWN");
}

TEST(CApi, RejectsBadArguments) {
  rda_transport *a = nullptr, *b = nullptr;
  EXPECT_EQ(rda_pipe_create(nullptr, &b), RDA_ERR_INVALID_ARG);
  EXPECT_NE(std::string(rda_last_error()), "");
  EXPECT_EQ(rda_host_create(nullptr, nullptr, nullptr), RDA_ERR_INVALID_ARG);
  EXPECT_EQ(rda_tcp_connect(nullptr, 1, &a), RDA_ERR_INVALID_ARG);

  // A bad config: the transport is still taken over (and freed).
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_host_config c;
  rda_host_config_default(&c);
  c.fps = 0;
  rda_host* h = nullptr;
  EXPECT_EQ(rda_host_create(a, &c, &h), RDA_ERR_INVALID_ARG);
  EXPECT_EQ(h, nullptr);
  EXPECT_NE(std::string(rda_last_error()).find("fps"), std::string::npos);

  rda_host_config_default(&c);
  c.capture_width = 1279;
  c.capture_height = 720;
  EXPECT_EQ(rda_host_create(b, &c, &h), RDA_ERR_INVALID_ARG);

  // Null objects are ignored by the void functions.
  rda_host_destroy(nullptr);
  rda_viewer_destroy(nullptr);
  rda_transport_destroy(nullptr);
  rda_host_request_keyframe(nullptr);
  char name[8] = "x";
  EXPECT_EQ(rda_host_encoder_name(nullptr, name, sizeof(name)), 0u);
  EXPECT_STREQ(name, "");
}

TEST(CApi, SubmitFrameChecksTheImage) {
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_transport_destroy(b);
  rda_host* h = nullptr;
  ASSERT_EQ(rda_host_create(a, nullptr, &h), RDA_OK);  // NULL config = defaults
  std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4);
  rda_image img{px.data(), kW - 1, kH, kW * 4};  // odd width
  EXPECT_EQ(rda_host_submit_frame(h, &img, 0, nullptr, 0), RDA_ERR_INVALID_ARG);
  img = {px.data(), kW, kH, kW * 2};  // stride too small
  EXPECT_EQ(rda_host_submit_frame(h, &img, 0, nullptr, 0), RDA_ERR_INVALID_ARG);
  img = {px.data(), kW, kH, kW * 4};
  EXPECT_EQ(rda_host_submit_frame(h, &img, 0, nullptr, 3), RDA_ERR_INVALID_ARG);  // null dirty
  rda_host_destroy(h);
}

TEST(CApi, OlderShorterStructsAreAccepted) {
  // A program built against an older header passes a shorter config; the
  // fields it does not know keep their defaults.
  rda_host_config full;
  rda_host_config_default(&full);
  EXPECT_EQ(full.capture_own_windows, 0);  // 1.1; off unless asked for
  full.fps = 30;
  rda_host_config old = full;
  old.struct_size = offsetof(rda_host_config, lossless);  // ends before `lossless`
  old.lossless = 12345;                                    // must be ignored
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_transport_destroy(b);
  rda_host* h = nullptr;
  ASSERT_EQ(rda_host_create(a, &old, &h), RDA_OK);
  rda_host_destroy(h);

  // A 1.0 program: its config ends before capture_own_windows.
  rda_host_config v10 = full;
  v10.struct_size = offsetof(rda_host_config, capture_own_windows);
  v10.capture_own_windows = 7;  // beyond its struct: must be ignored
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_transport_destroy(b);
  ASSERT_EQ(rda_host_create(a, &v10, &h), RDA_OK);
  rda_host_destroy(h);

  // Stats into a shorter struct: only the known prefix is written.
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_transport_destroy(b);
  ASSERT_EQ(rda_host_create(a, nullptr, &h), RDA_OK);
  struct {
    rda_host_stats s;
    uint64_t guard;
  } buf;
  std::memset(&buf, 0xAB, sizeof(buf));
  buf.s.struct_size = offsetof(rda_host_stats, frames_skipped);
  ASSERT_EQ(rda_host_get_stats(h, &buf.s), RDA_OK);
  EXPECT_EQ(buf.s.struct_size, offsetof(rda_host_stats, frames_skipped));
  EXPECT_EQ(buf.s.frames_captured, 0u);
  uint64_t untouched;
  std::memset(&untouched, 0xAB, sizeof(untouched));
  EXPECT_EQ(buf.s.frames_skipped, untouched);
  rda_host_destroy(h);

  rda_host_stats zero{};
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_transport_destroy(b);
  ASSERT_EQ(rda_host_create(a, nullptr, &h), RDA_OK);
  EXPECT_EQ(rda_host_get_stats(h, &zero), RDA_ERR_INVALID_ARG);  // struct_size 0
  rda_host_destroy(h);
}

TEST(CApi, HostToViewerWithInput) {
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  Counter got;

  rda_viewer_config vc;
  rda_viewer_config_default(&vc);
  vc.user = &got;
  vc.on_frame = [](void* u, const rda_image* img, const rda_rect*, size_t, const rda_frame_info* info) {
    EXPECT_EQ(img->width, kW);
    EXPECT_GE(img->stride, kW * 4);
    EXPECT_GT(info->frame_id, 0u);
    static_cast<Counter*>(u)->frames++;
  };
  rda_viewer* v = nullptr;
  ASSERT_EQ(rda_viewer_create(b, &vc, &v), RDA_OK) << rda_last_error();

  rda_host_config hc;
  rda_host_config_default(&hc);
  hc.user = &got;
  hc.on_input = [](void* u, const rda_input_event* ev) {
    auto* c = static_cast<Counter*>(u);
    c->last_code = ev->code;
    c->inputs++;
  };
  rda_host* h = nullptr;
  ASSERT_EQ(rda_host_create(a, &hc, &h), RDA_OK) << rda_last_error();

  std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4);
  for (int i = 0; i < 5; ++i) {
    rda_rect dirty[16];
    size_t n = 0;
    ASSERT_EQ(rda_test_pattern_render(i, kW, kH, px.data(), i > 0, dirty, 16, &n), RDA_OK);
    const rda_image img{px.data(), kW, kH, kW * 4};
    ASSERT_EQ(rda_host_submit_frame(h, &img, rda_now_us(), dirty, n), RDA_OK) << rda_last_error();
  }
  EXPECT_TRUE(WaitUntil([&] { return got.frames == 5; }));

  rda_input_event ev;
  rda_input_event_default(&ev);
  ev.kind = RDA_INPUT_KEY_DOWN;
  ev.code = 4;
  ASSERT_EQ(rda_viewer_send_input(v, &ev), RDA_OK);
  ev.kind = static_cast<rda_input_kind>(99);
  EXPECT_EQ(rda_viewer_send_input(v, &ev), RDA_ERR_INVALID_ARG);
  EXPECT_TRUE(WaitUntil([&] { return got.inputs == 1; }));
  EXPECT_EQ(got.last_code, 4u);
  ev.kind = RDA_INPUT_TEXT;
  ev.code = 0xD800;  // a surrogate is not a character
  EXPECT_EQ(rda_viewer_send_input(v, &ev), RDA_ERR_INVALID_ARG);
  ev.code = 0x4E2D;
  ASSERT_EQ(rda_viewer_send_input(v, &ev), RDA_OK) << rda_last_error();
  EXPECT_TRUE(WaitUntil([&] { return got.inputs == 2; }));
  EXPECT_EQ(got.last_code, 0x4E2Du);

  char enc[64];
  EXPECT_GT(rda_host_encoder_name(h, enc, sizeof(enc)), 0u);
  rda_viewer_stats vs{};
  vs.struct_size = sizeof(vs);
  ASSERT_EQ(rda_viewer_get_stats(v, &vs), RDA_OK);
  EXPECT_EQ(vs.frames_decoded, 5u);

  rda_host_destroy(h);
  rda_viewer_destroy(v);
}

// A transport written against the C vtable: an in-memory loop between two
// instances, delivering on a thread of its own.
namespace {

struct LoopTransport {
  std::mutex mu;
  LoopTransport* peer = nullptr;
  rda_receive_fn fn = nullptr;
  void* ctx = nullptr;
  std::vector<std::pair<rda_channel, std::vector<uint8_t>>> pending;  // before a receiver is set
  bool closed = false;
  std::atomic<int>* destroyed = nullptr;

  static rda_status Send(void* self, rda_channel ch, const uint8_t* p, size_t n) {
    auto* t = static_cast<LoopTransport*>(self);
    LoopTransport* peer = t->peer;
    std::lock_guard<std::mutex> lock(peer->mu);
    if (t->closed || peer->closed) return RDA_ERR_CLOSED;
    if (peer->fn) {
      peer->fn(peer->ctx, ch, p, n);  // synchronous delivery
    } else {
      peer->pending.push_back({ch, std::vector<uint8_t>(p, p + n)});
    }
    return RDA_OK;
  }
  static void SetReceiver(void* self, rda_receive_fn fn, void* ctx) {
    auto* t = static_cast<LoopTransport*>(self);
    std::lock_guard<std::mutex> lock(t->mu);
    t->fn = fn;
    t->ctx = ctx;
    if (fn) {
      for (auto& [ch, bytes] : t->pending) fn(ctx, ch, bytes.data(), bytes.size());
      t->pending.clear();
    }
  }
  static void Close(void* self) {
    auto* t = static_cast<LoopTransport*>(self);
    std::lock_guard<std::mutex> lock(t->mu);
    t->closed = true;
  }
  static int Stats(void*, rda_transport_stats* out) {
    out->bandwidth_bps = 5'000'000;
    out->rtt_us = 1000;
    out->packets_sent = 200;
    out->packets_lost = 3;
    return 1;
  }
  static void Destroy(void* self) {
    auto* t = static_cast<LoopTransport*>(self);
    (*t->destroyed)++;
    // The peer is freed by its own destroy; just unhook.
    delete t;
  }
};

const rda_transport_vtable kLoopVtable = {sizeof(rda_transport_vtable), LoopTransport::Send,
                                          LoopTransport::SetReceiver, LoopTransport::Close,
                                          LoopTransport::Stats, LoopTransport::Destroy};

}  // namespace

TEST(CApi, CustomTransportThroughVtable) {
  std::atomic<int> destroyed{0};
  auto* x = new LoopTransport;
  auto* y = new LoopTransport;
  x->peer = y;
  y->peer = x;
  x->destroyed = y->destroyed = &destroyed;
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_transport_custom(&kLoopVtable, x, &a), RDA_OK);
  ASSERT_EQ(rda_transport_custom(&kLoopVtable, y, &b), RDA_OK);

  rda_transport_stats st{};
  st.struct_size = sizeof(st);
  EXPECT_EQ(rda_transport_get_stats(a, &st), 1);
  EXPECT_EQ(st.bandwidth_bps, 5'000'000);

  Counter got;
  rda_viewer_config vc;
  rda_viewer_config_default(&vc);
  vc.user = &got;
  vc.on_frame = [](void* u, const rda_image*, const rda_rect*, size_t, const rda_frame_info*) {
    static_cast<Counter*>(u)->frames++;
  };
  rda_viewer* v = nullptr;
  ASSERT_EQ(rda_viewer_create(b, &vc, &v), RDA_OK);
  rda_host* h = nullptr;
  ASSERT_EQ(rda_host_create(a, nullptr, &h), RDA_OK);
  std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4);
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(rda_test_pattern_render(i, kW, kH, px.data(), i > 0, nullptr, 0, nullptr), RDA_OK);
    const rda_image img{px.data(), kW, kH, kW * 4};
    ASSERT_EQ(rda_host_submit_frame(h, &img, rda_now_us(), nullptr, 0), RDA_OK) << rda_last_error();
  }
  // Decoded on the viewer's own thread.
  for (int i = 0; i < 500 && got.frames < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(got.frames, 3);
  rda_host_stats hs{};
  hs.struct_size = sizeof(hs);
  ASSERT_EQ(rda_host_get_stats(h, &hs), RDA_OK);
  EXPECT_EQ(hs.bandwidth_estimate_bps, 5'000'000);  // the host read our stats
  EXPECT_EQ(hs.packets_sent, 200u);
  EXPECT_EQ(hs.packets_lost, 3u);

  // Destroy the host first; then x is gone and y must not call into it.
  rda_host_destroy(h);
  y->peer = y;  // the viewer may still send keyframe requests; loop them back harmlessly
  y->closed = true;
  rda_viewer_destroy(v);
  EXPECT_EQ(destroyed, 2);

  // A vtable missing required functions: destroy is still called.
  rda_transport_vtable bad = kLoopVtable;
  bad.send = nullptr;
  auto* z = new LoopTransport;
  z->destroyed = &destroyed;
  rda_transport* c = nullptr;
  EXPECT_EQ(rda_transport_custom(&bad, z, &c), RDA_ERR_INVALID_ARG);
  EXPECT_EQ(destroyed, 3);
}

TEST(CApi, LogSinkGetsLibraryMessages) {
  struct Sink {
    std::mutex mu;
    std::vector<std::string> lines;
  } sink;
  rda_set_log(
      [](void* u, rda_log_level, const char* m) {
        auto* s = static_cast<Sink*>(u);
        std::lock_guard<std::mutex> lock(s->mu);
        s->lines.push_back(m);
      },
      &sink, RDA_LOG_DEBUG);

  // An encoder that cannot exist produces an error on the host.
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_transport_destroy(b);
  rda_host_config hc;
  rda_host_config_default(&hc);
  hc.encoder = "no_such_encoder";
  struct Err {
    std::atomic<int> n{0};
    rda_status last = RDA_OK;
  } err;
  hc.user = &err;
  hc.on_error = [](void* u, rda_status s, const char*) {
    auto* e = static_cast<Err*>(u);
    e->last = s;
    e->n++;
  };
  rda_host* h = nullptr;
  ASSERT_EQ(rda_host_create(a, &hc, &h), RDA_OK);
  std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4, 0);
  const rda_image img{px.data(), kW, kH, kW * 4};
  // Linux falls back to the other backends, macOS ignores the name: either
  // way the frame encodes or fails cleanly with a status.
  const rda_status s = rda_host_submit_frame(h, &img, rda_now_us(), nullptr, 0);
  EXPECT_TRUE(s == RDA_OK || s == RDA_ERR_CODEC || s == RDA_ERR_UNSUPPORTED) << rda_status_str(s);
  if (s != RDA_OK) {
    EXPECT_EQ(err.n, 1);
    std::lock_guard<std::mutex> lock(sink.mu);
    EXPECT_FALSE(sink.lines.empty());
  }
  rda_host_destroy(h);
  rda_set_log(nullptr, nullptr, RDA_LOG_ERROR);
}

TEST(CApi, RecordingRoundTripAndFree) {
  const std::string path = ::testing::TempDir() + "rdaccel_capi.rdstream";
  rda_transport* file = nullptr;
  ASSERT_EQ(rda_recording_create(path.c_str(), &file), RDA_OK);
  rda_host* h = nullptr;
  ASSERT_EQ(rda_host_create(file, nullptr, &h), RDA_OK);
  std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(rda_test_pattern_render(i, kW, kH, px.data(), i > 0, nullptr, 0, nullptr), RDA_OK);
    const rda_image img{px.data(), kW, kH, kW * 4};
    ASSERT_EQ(rda_host_submit_frame(h, &img, i * 16'000ull, nullptr, 0), RDA_OK);
  }
  rda_host_destroy(h);

  uint8_t* bytes = nullptr;
  size_t n = 0;
  ASSERT_EQ(rda_recording_read(path.c_str(), &bytes, &n), RDA_OK) << rda_last_error();
  EXPECT_GT(n, 100u);
  Counter got;
  rda_viewer_config vc;
  rda_viewer_config_default(&vc);
  vc.user = &got;
  vc.on_frame = [](void* u, const rda_image*, const rda_rect*, size_t, const rda_frame_info*) {
    static_cast<Counter*>(u)->frames++;
  };
  rda_viewer* v = nullptr;
  ASSERT_EQ(rda_viewer_create(nullptr, &vc, &v), RDA_OK);
  ASSERT_EQ(rda_viewer_feed_recording(v, bytes, n), RDA_OK);
  EXPECT_EQ(got.frames, 4);
  rda_viewer_destroy(v);
  rda_free(bytes);
  std::remove(path.c_str());
  EXPECT_EQ(rda_recording_read(path.c_str(), &bytes, &n), RDA_ERR_IO);
  EXPECT_EQ(bytes, nullptr);
}

namespace {

struct ConnEvents {
  std::mutex mu;
  std::vector<std::pair<rda_state, rda_status>> states;
  std::vector<std::pair<uint32_t, std::string>> messages;
  std::string token;
  int accept = 1;

  static void OnState(void* u, rda_state s, rda_status r) {
    auto* e = static_cast<ConnEvents*>(u);
    std::lock_guard<std::mutex> lock(e->mu);
    e->states.emplace_back(s, r);
  }
  static void OnMessage(void* u, uint32_t type, const uint8_t* p, size_t n) {
    auto* e = static_cast<ConnEvents*>(u);
    std::lock_guard<std::mutex> lock(e->mu);
    e->messages.emplace_back(type, std::string(reinterpret_cast<const char*>(p), n));
  }
  static int OnAuth(void* u, const uint8_t* t, size_t n) {
    auto* e = static_cast<ConnEvents*>(u);
    std::lock_guard<std::mutex> lock(e->mu);
    e->token.assign(reinterpret_cast<const char*>(t), n);
    return e->accept;
  }
  bool Has(rda_state s) {
    std::lock_guard<std::mutex> lock(mu);
    for (auto& st : states) {
      if (st.first == s) return true;
    }
    return false;
  }
  rda_status LastReason() {
    std::lock_guard<std::mutex> lock(mu);
    return states.empty() ? RDA_OK : states.back().second;
  }
  size_t MessageCount() {
    std::lock_guard<std::mutex> lock(mu);
    return messages.size();
  }
};

// A host and a viewer over a pipe with all connection callbacks.
struct ConnPair {
  ConnEvents host_ev, viewer_ev;
  rda_host* host = nullptr;
  rda_viewer* viewer = nullptr;
  std::string token = "secret";

  rda_status Create(int accept) {
    host_ev.accept = accept;
    rda_transport *a = nullptr, *b = nullptr;
    if (rda_pipe_create(&a, &b) != RDA_OK) return RDA_ERR_TRANSPORT;
    rda_host_config hc;
    rda_host_config_default(&hc);
    hc.user = &host_ev;
    hc.on_state = &ConnEvents::OnState;
    hc.on_message = &ConnEvents::OnMessage;
    hc.on_auth = &ConnEvents::OnAuth;
    if (rda_status s = rda_host_create(a, &hc, &host); s != RDA_OK) {
      rda_transport_destroy(b);
      return s;
    }
    rda_viewer_config vc;
    rda_viewer_config_default(&vc);
    vc.user = &viewer_ev;
    vc.on_state = &ConnEvents::OnState;
    vc.on_message = &ConnEvents::OnMessage;
    vc.auth_token = reinterpret_cast<const uint8_t*>(token.data());
    vc.auth_token_len = token.size();
    return rda_viewer_create(b, &vc, &viewer);
  }
  ~ConnPair() {
    rda_viewer_destroy(viewer);
    rda_host_destroy(host);
  }
};

}  // namespace

TEST(CApi, ConnectionStateMessagesAndGoodbye) {
  ConnPair p;
  ASSERT_EQ(p.Create(1), RDA_OK) << rda_last_error();
  ASSERT_TRUE(WaitUntil([&] { return p.host_ev.Has(RDA_STATE_CONNECTED); }));
  ASSERT_TRUE(WaitUntil([&] { return p.viewer_ev.Has(RDA_STATE_CONNECTED); }));
  EXPECT_EQ(rda_host_get_state(p.host), RDA_STATE_CONNECTED);
  EXPECT_EQ(rda_viewer_get_state(p.viewer), RDA_STATE_CONNECTED);
  EXPECT_EQ(p.host_ev.token, "secret");

  const std::string clip = "hello clipboard";
  ASSERT_EQ(rda_viewer_send_message(p.viewer, RDA_MSG_CLIPBOARD_TEXT, reinterpret_cast<const uint8_t*>(clip.data()),
                                    clip.size()),
            RDA_OK);
  const uint8_t user[3] = {'a', 'b', 'c'};
  ASSERT_EQ(rda_host_send_message(p.host, RDA_MSG_USER_FIRST + 1, user, 3), RDA_OK);
  EXPECT_EQ(rda_host_send_message(p.host, 2, user, 3), RDA_ERR_INVALID_ARG);
  EXPECT_EQ(rda_host_send_message(p.host, RDA_MSG_USER_FIRST, nullptr, 3), RDA_ERR_INVALID_ARG);
  ASSERT_TRUE(WaitUntil([&] { return p.host_ev.MessageCount() == 1 && p.viewer_ev.MessageCount() == 1; }));
  EXPECT_EQ(p.host_ev.messages[0].first, RDA_MSG_CLIPBOARD_TEXT);
  EXPECT_EQ(p.host_ev.messages[0].second, clip);
  EXPECT_EQ(p.viewer_ev.messages[0].first, RDA_MSG_USER_FIRST + 1);
  EXPECT_EQ(p.viewer_ev.messages[0].second, "abc");

  // The viewer leaves: the host hears goodbye, and further sends fail.
  rda_viewer_destroy(p.viewer);
  p.viewer = nullptr;
  ASSERT_TRUE(WaitUntil([&] { return p.host_ev.Has(RDA_STATE_DISCONNECTED); }));
  EXPECT_EQ(p.host_ev.LastReason(), RDA_ERR_CLOSED);
  EXPECT_EQ(rda_host_send_message(p.host, RDA_MSG_USER_FIRST, user, 3), RDA_ERR_CLOSED);
  std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4, 0);
  const rda_image img{px.data(), kW, kH, kW * 4};
  EXPECT_EQ(rda_host_submit_frame(p.host, &img, 0, nullptr, 0), RDA_ERR_CLOSED);
}

TEST(CApi, AuthHookRejects) {
  ConnPair p;
  ASSERT_EQ(p.Create(0), RDA_OK) << rda_last_error();
  ASSERT_TRUE(WaitUntil([&] { return p.viewer_ev.Has(RDA_STATE_DISCONNECTED); }));
  EXPECT_EQ(p.viewer_ev.LastReason(), RDA_ERR_AUTH);
  ASSERT_TRUE(WaitUntil([&] { return p.host_ev.Has(RDA_STATE_DISCONNECTED); }));
  EXPECT_EQ(p.host_ev.LastReason(), RDA_ERR_AUTH);
  EXPECT_FALSE(p.viewer_ev.Has(RDA_STATE_CONNECTED));
}

namespace {

struct Leak {
  std::atomic<int> stream_info{0}, frames{0}, cursor{0}, connected{0}, disconnected{0};
  static void OnStreamInfo(void* u, int32_t, int32_t) { ++static_cast<Leak*>(u)->stream_info; }
  static void OnFrame(void* u, const rda_image*, const rda_rect*, size_t, const rda_frame_info*) {
    ++static_cast<Leak*>(u)->frames;
  }
  static void OnCursor(void* u, const rda_cursor_state*) { ++static_cast<Leak*>(u)->cursor; }
  static void OnState(void* u, rda_state s, rda_status) {
    if (s == RDA_STATE_CONNECTED) ++static_cast<Leak*>(u)->connected;
    if (s == RDA_STATE_DISCONNECTED) ++static_cast<Leak*>(u)->disconnected;
  }
};

int Refuse(void*, const uint8_t*, size_t) { return 0; }
int Accept(void*, const uint8_t*, size_t) { return 1; }

}  // namespace

// A viewer the auth hook refuses learns nothing about the screen: not its
// size, not a picture, not the pointer, even when the host has been
// streaming (into a transport that holds the bytes) before it came.
TEST(CApi, RefusedViewerSeesNothingOfTheScreen) {
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_host_config hc;
  rda_host_config_default(&hc);
  hc.on_auth = &Refuse;
  rda_host* host = nullptr;
  ASSERT_EQ(rda_host_create(a, &hc, &host), RDA_OK) << rda_last_error();
  std::vector<uint8_t> px;
  rda_rect rects[16];
  size_t nr = 0;
  px.resize(static_cast<size_t>(kW) * kH * 4);
  for (uint64_t i = 0; i < 3; ++i) {
    ASSERT_EQ(rda_test_pattern_render(i, kW, kH, px.data(), i > 0, rects, 16, &nr), RDA_OK);
    const rda_image img{px.data(), kW, kH, kW * 4};
    ASSERT_EQ(rda_host_submit_frame(host, &img, rda_now_us(), nullptr, 0), RDA_OK) << rda_last_error();
  }
  std::vector<uint8_t> cur(16 * 16 * 4, 0xff);
  const rda_cursor_image ci{16, 16, 0, 0, cur.data()};
  ASSERT_EQ(rda_host_set_cursor_image(host, &ci), RDA_OK);
  rda_host_set_cursor(host, 10, 20, 1, 0);

  Leak got;
  rda_viewer_config vc;
  rda_viewer_config_default(&vc);
  vc.user = &got;
  vc.on_stream_info = &Leak::OnStreamInfo;
  vc.on_frame = &Leak::OnFrame;
  vc.on_cursor = &Leak::OnCursor;
  vc.on_state = &Leak::OnState;
  rda_viewer* viewer = nullptr;
  ASSERT_EQ(rda_viewer_create(b, &vc, &viewer), RDA_OK) << rda_last_error();
  ASSERT_TRUE(WaitUntil([&] { return got.disconnected.load() > 0; }));
  EXPECT_EQ(rda_viewer_get_state(viewer), RDA_STATE_DISCONNECTED);
  rda_host_set_cursor(host, 30, 40, 1, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // anything late
  rda_viewer_destroy(viewer);
  rda_host_destroy(host);
  EXPECT_EQ(got.connected.load(), 0);
  EXPECT_EQ(got.stream_info.load(), 0);
  EXPECT_EQ(got.frames.load(), 0);
  EXPECT_EQ(got.cursor.load(), 0);
}

// The same host, accepted: it gets the size, a full picture and the cursor
// that were current before it connected.
TEST(CApi, ViewerArrivingLateGetsScreenAndCursor) {
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_host_config hc;
  rda_host_config_default(&hc);
  hc.on_auth = &Accept;
  rda_host* host = nullptr;
  ASSERT_EQ(rda_host_create(a, &hc, &host), RDA_OK) << rda_last_error();
  std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4);
  rda_rect rects[16];
  size_t nr = 0;
  ASSERT_EQ(rda_test_pattern_render(0, kW, kH, px.data(), 0, rects, 16, &nr), RDA_OK);
  const rda_image img{px.data(), kW, kH, kW * 4};
  ASSERT_EQ(rda_host_submit_frame(host, &img, rda_now_us(), nullptr, 0), RDA_OK) << rda_last_error();
  std::vector<uint8_t> cur(16 * 16 * 4, 0xff);
  const rda_cursor_image ci{16, 16, 0, 0, cur.data()};
  ASSERT_EQ(rda_host_set_cursor_image(host, &ci), RDA_OK);
  rda_host_set_cursor(host, 10, 20, 1, 0);

  Leak got;
  rda_viewer_config vc;
  rda_viewer_config_default(&vc);
  vc.user = &got;
  vc.on_stream_info = &Leak::OnStreamInfo;
  vc.on_frame = &Leak::OnFrame;
  vc.on_cursor = &Leak::OnCursor;
  vc.on_state = &Leak::OnState;
  rda_viewer* viewer = nullptr;
  ASSERT_EQ(rda_viewer_create(b, &vc, &viewer), RDA_OK) << rda_last_error();
  // No new frame: the host re-sends the one it kept.
  EXPECT_TRUE(WaitUntil([&] { return got.frames.load() > 0 && got.cursor.load() > 0; }));
  EXPECT_EQ(got.connected.load(), 1);
  EXPECT_EQ(got.stream_info.load(), 1);
  rda_viewer_destroy(viewer);
  rda_host_destroy(host);
}

namespace {

struct HandleSeen {
  rda_viewer** viewer = nullptr;
  rda_host** host = nullptr;
  std::atomic<int> null_seen{0}, calls{0};
  static void OnViewerState(void* u, rda_state, rda_status) {
    auto* h = static_cast<HandleSeen*>(u);
    ++h->calls;
    if (!*h->viewer) ++h->null_seen;
  }
  static void OnHostState(void* u, rda_state, rda_status) {
    auto* h = static_cast<HandleSeen*>(u);
    ++h->calls;
    if (!*h->host) ++h->null_seen;
  }
};

}  // namespace

// Callbacks may use the handle from the variable create writes: none runs
// before create has returned, even when the peer is already there.
TEST(CApi, CallbacksSeeTheHandleCreateReturned) {
  for (int round = 0; round < 20; ++round) {
    rda_transport *a = nullptr, *b = nullptr;
    ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
    rda_viewer* viewer = nullptr;
    rda_host* host = nullptr;
    HandleSeen seen;
    seen.viewer = &viewer;
    seen.host = &host;
    rda_viewer_config vc;
    rda_viewer_config_default(&vc);
    vc.user = &seen;
    vc.on_state = &HandleSeen::OnViewerState;
    // The viewer's hello is waiting in the pipe before the host exists.
    ASSERT_EQ(rda_viewer_create(b, &vc, &viewer), RDA_OK);
    rda_host_config hc;
    rda_host_config_default(&hc);
    hc.user = &seen;
    hc.on_state = &HandleSeen::OnHostState;
    ASSERT_EQ(rda_host_create(a, &hc, &host), RDA_OK);
    ASSERT_TRUE(WaitUntil([&] { return seen.calls.load() >= 2; }));
    rda_viewer_destroy(viewer);
    rda_host_destroy(host);
    EXPECT_EQ(seen.null_seen.load(), 0) << "round " << round;
  }
}

TEST(CApi, KeyCodes) {
  EXPECT_EQ(rda_keycode_from_mac(0x00), 0x04u);
  EXPECT_EQ(rda_keycode_to_evdev(0x04), 30u);
  EXPECT_EQ(rda_keycode_from_evdev(125), 0xE3u);
  EXPECT_EQ(rda_keycode_to_mac(0xE3), 0x37u);
  EXPECT_EQ(rda_keycode_to_mac(0x46), RDA_KEYCODE_NONE);
}

TEST(CApi, NetworkTransportsNeedAKeyOrExplicitPlaintext) {
  rda_transport* t = nullptr;
  ASSERT_EQ(rda_udp_bind(47321, &t), RDA_OK) << rda_last_error();
  rda_host* h = nullptr;
  EXPECT_EQ(rda_host_create(t, nullptr, &h), RDA_ERR_INVALID_ARG);  // t is taken over and freed
  EXPECT_NE(std::string(rda_last_error()).find("allow_plaintext"), std::string::npos);

  const uint8_t key[32] = {7};
  rda_host_config c;
  rda_host_config_default(&c);
  c.encryption_key = key;
  ASSERT_EQ(rda_udp_bind(47322, &t), RDA_OK);
  ASSERT_EQ(rda_host_create(t, &c, &h), RDA_OK) << rda_last_error();
  rda_host_destroy(h);

  rda_host_config_default(&c);
  c.allow_plaintext = 1;
  ASSERT_EQ(rda_udp_bind(47323, &t), RDA_OK);
  ASSERT_EQ(rda_host_create(t, &c, &h), RDA_OK) << rda_last_error();
  rda_host_destroy(h);

  // A pipe never needs either.
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_transport_destroy(b);
  ASSERT_EQ(rda_host_create(a, nullptr, &h), RDA_OK);
  rda_host_destroy(h);
  EXPECT_EQ(rda_udp_from_fd(-1, 0, &t), RDA_ERR_INVALID_ARG);
}

TEST(CApi, InjectorCreateAndArguments) {
  rda_injector* inj = nullptr;
  EXPECT_EQ(rda_injector_create(nullptr, nullptr), RDA_ERR_INVALID_ARG);
  rda_injector_config c;
  rda_injector_config_default(&c);
  c.display_index = -1;
  EXPECT_EQ(rda_injector_create(&c, &inj), RDA_ERR_INVALID_ARG);
  const rda_status st = rda_injector_create(nullptr, &inj);
  if (st == RDA_ERR_UNSUPPORTED || st == RDA_ERR_PERMISSION) GTEST_SKIP() << rda_last_error();
  ASSERT_EQ(st, RDA_OK) << rda_last_error();
  ASSERT_NE(inj, nullptr);
  EXPECT_EQ(rda_injector_inject(inj, nullptr), RDA_ERR_INVALID_ARG);
  rda_input_event ev;
  rda_input_event_default(&ev);
  ev.kind = static_cast<rda_input_kind>(99);
  EXPECT_EQ(rda_injector_inject(inj, &ev), RDA_ERR_INVALID_ARG);
  ev.kind = RDA_INPUT_MOUSE_DOWN;
  ev.code = 77;  // no such button
  EXPECT_EQ(rda_injector_inject(inj, &ev), RDA_ERR_UNSUPPORTED);
  ev.kind = RDA_INPUT_MOUSE_MOVE;
  ev.x = 3;
  ev.y = 4;
  EXPECT_EQ(rda_injector_inject(inj, &ev), RDA_OK) << rda_last_error();
  rda_injector_destroy(inj);
  rda_injector_destroy(nullptr);
}

TEST(CApi, ListDisplaysAndPickOne) {
  size_t n = 0;
  EXPECT_EQ(rda_list_displays(nullptr, 1, &n), RDA_ERR_INVALID_ARG);
  const rda_status s = rda_list_displays(nullptr, 0, &n);
  if (s == RDA_ERR_UNSUPPORTED) GTEST_SKIP() << rda_last_error();  // no display here
  ASSERT_EQ(s, RDA_OK) << rda_last_error();
  ASSERT_GE(n, 1u);
  // An older, shorter struct: entries are written at its stride.
  struct Old {
    uint32_t struct_size;
    int32_t index, x, y, width, height;
  };
  std::vector<Old> old(n + 1, Old{0, -1, 0, 0, 0, 0});
  old[0].struct_size = sizeof(Old);
  size_t m = 0;
  ASSERT_EQ(rda_list_displays(reinterpret_cast<rda_display_info*>(old.data()), n, &m), RDA_OK);
  EXPECT_EQ(m, n);
  for (size_t i = 0; i < n; ++i) {
    EXPECT_EQ(old[i].index, static_cast<int32_t>(i));
    EXPECT_GT(old[i].width, 0);
  }
  EXPECT_EQ(old[n].index, -1);  // untouched past capacity

  // A display that does not exist fails at capture start.
  rda_transport *a = nullptr, *b = nullptr;
  ASSERT_EQ(rda_pipe_create(&a, &b), RDA_OK);
  rda_transport_destroy(b);
  rda_host_config c;
  rda_host_config_default(&c);
  EXPECT_EQ(c.display_index, 0);
  c.display_index = static_cast<int32_t>(n);
  rda_host* h = nullptr;
  ASSERT_EQ(rda_host_create(a, &c, &h), RDA_OK) << rda_last_error();
  EXPECT_NE(rda_host_start_capture(h), RDA_OK);
  EXPECT_NE(std::string(rda_last_error()).find("no display"), std::string::npos) << rda_last_error();
  rda_host_destroy(h);
}
