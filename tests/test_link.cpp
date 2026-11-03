// The connection layer (session/link.h) over an in-process pipe: handshake,
// version checks both ways, auth, keepalive timeout, goodbye, and
// application messages. No codec involved.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "proto/messages.h"
#include "rdacore/transport.h"
#include "session/link.h"

using namespace rdacore;
using session::Link;
using session::LinkError;
using session::LinkOptions;
using session::LinkState;

namespace {

bool WaitUntil(const std::function<bool()>& done, int ms = 5000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

// Records what one side's Link reports.
struct Events {
  std::mutex mu;
  std::vector<std::pair<LinkState, LinkError>> states;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> messages;

  Link::Handlers Handlers() {
    Link::Handlers h;
    h.on_state = [this](LinkState s, LinkError e) {
      std::lock_guard<std::mutex> lock(mu);
      states.emplace_back(s, e);
    };
    h.on_message = [this](uint32_t type, std::span<const uint8_t> p) {
      std::lock_guard<std::mutex> lock(mu);
      messages.emplace_back(type, std::vector<uint8_t>(p.begin(), p.end()));
    };
    return h;
  }
  size_t message_count() {
    std::lock_guard<std::mutex> lock(mu);
    return messages.size();
  }
  std::vector<std::pair<LinkState, LinkError>> state_log() {
    std::lock_guard<std::mutex> lock(mu);
    return states;
  }
};

// A pipe end used directly: collects the control messages the peer sends.
struct RawPeer {
  std::unique_ptr<Transport> t;
  std::mutex mu;
  proto::StreamParser parser;
  std::vector<proto::Message> control;

  explicit RawPeer(std::unique_ptr<Transport> tr) : t(std::move(tr)) {
    t->SetReceiver([this](Channel ch, std::span<const uint8_t> b) {
      if (ch != Channel::kControl) return;
      std::lock_guard<std::mutex> lock(mu);
      parser.Append(b);
      while (auto m = parser.Next()) control.push_back(std::move(*m));
    });
  }
  ~RawPeer() { t.reset(); }
  void Send(const proto::Message& m) { t->Send(Channel::kControl, proto::Encode(m)); }
  template <class T>
  std::optional<T> Find() {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& m : control) {
      if (auto* p = std::get_if<T>(&m)) return *p;
    }
    return std::nullopt;
  }
};

// A host and a viewer Link on the two ends of a pipe.
struct Pair {
  std::unique_ptr<Transport> host_t, viewer_t;
  Events host_ev, viewer_ev;
  std::unique_ptr<Link> host, viewer;

  explicit Pair(LinkOptions host_opts = {}, LinkOptions viewer_opts = {},
                std::function<bool(std::span<const uint8_t>)> auth = nullptr) {
    auto [a, b] = CreateLocalPipe();
    host_t = std::move(a);
    viewer_t = std::move(b);
    auto hh = host_ev.Handlers();
    hh.on_auth = std::move(auth);
    host = std::make_unique<Link>(Link::Role::kHost, host_t.get(), host_opts, std::move(hh));
    viewer = std::make_unique<Link>(Link::Role::kViewer, viewer_t.get(), viewer_opts, viewer_ev.Handlers());
    host_t->SetReceiver([this](Channel ch, std::span<const uint8_t> b) { host->OnReceive(ch, b); });
    viewer_t->SetReceiver([this](Channel ch, std::span<const uint8_t> b) { viewer->OnReceive(ch, b); });
    host->Start();
    viewer->Start();
  }
  ~Pair() {
    viewer->Shutdown();
    host->Shutdown();
    viewer_t.reset();
    host_t.reset();
  }
};

}  // namespace

TEST(Link, HandshakeConnectsBothSides) {
  Pair p;
  ASSERT_TRUE(WaitUntil([&] { return p.host->state() == LinkState::kConnected; }));
  ASSERT_TRUE(WaitUntil([&] { return p.viewer->state() == LinkState::kConnected; }));
  EXPECT_EQ(p.host_ev.state_log().size(), 1u);
  EXPECT_EQ(p.viewer_ev.state_log().size(), 1u);
}

TEST(Link, MessagesBothWaysIncludingLargeOnes) {
  Pair p;
  ASSERT_TRUE(WaitUntil([&] { return p.viewer->state() == LinkState::kConnected; }));
  std::vector<uint8_t> big(1'500'000);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 31);
  const std::string text = "剪贴板 clipboard";
  std::string err;
  ASSERT_TRUE(p.viewer->SendMessage(kClipboardTextMessage,
                                    {reinterpret_cast<const uint8_t*>(text.data()), text.size()}, &err))
      << err;
  ASSERT_TRUE(p.viewer->SendMessage(kFirstUserMessage + 7, big, &err)) << err;
  ASSERT_TRUE(p.host->SendMessage(kFirstUserMessage, {}, &err)) << err;
  ASSERT_TRUE(WaitUntil([&] { return p.host_ev.message_count() == 2; }));
  ASSERT_TRUE(WaitUntil([&] { return p.viewer_ev.message_count() == 1; }));
  std::lock_guard<std::mutex> lock(p.host_ev.mu);
  EXPECT_EQ(p.host_ev.messages[0].first, kClipboardTextMessage);
  EXPECT_EQ(std::string(p.host_ev.messages[0].second.begin(), p.host_ev.messages[0].second.end()), text);
  EXPECT_EQ(p.host_ev.messages[1].first, kFirstUserMessage + 7);
  EXPECT_EQ(p.host_ev.messages[1].second, big);
}

TEST(Link, ReservedTypesAndOversizedMessagesAreRefused) {
  Pair p;
  std::string err;
  EXPECT_FALSE(p.host->SendMessage(0, {}, &err));
  EXPECT_FALSE(p.host->SendMessage(2, {}, &err));
  EXPECT_NE(err.find("reserved"), std::string::npos);
  EXPECT_FALSE(p.host->SendMessage(kFirstUserMessage - 1, {}, &err));
  std::vector<uint8_t> huge(proto::kMaxAppMessage + 1);
  EXPECT_FALSE(p.host->SendMessage(kFirstUserMessage, huge, &err));
  EXPECT_TRUE(p.host->SendMessage(kFirstUserMessage, std::span(huge).first(proto::kMaxAppMessage), &err))
      << err;
}

TEST(Link, CloseSaysGoodbye) {
  Pair p;
  ASSERT_TRUE(WaitUntil([&] { return p.viewer->state() == LinkState::kConnected; }));
  p.viewer->Close();
  EXPECT_EQ(p.viewer->error(), LinkError::kClosed);
  ASSERT_TRUE(WaitUntil([&] { return p.host->state() == LinkState::kDisconnected; }));
  EXPECT_EQ(p.host->error(), LinkError::kClosed);
  std::string err;
  EXPECT_FALSE(p.host->SendMessage(kFirstUserMessage, {}, &err));
  EXPECT_EQ(err.rfind("disconnected", 0), 0u) << err;
  // Disconnected is final: exactly connected, then disconnected.
  const auto log = p.host_ev.state_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[1].first, LinkState::kDisconnected);
}

TEST(Link, AuthHookCanRejectAViewer) {
  LinkOptions vo;
  vo.auth_token = {'n', 'o', 'p', 'e'};
  std::vector<uint8_t> seen;
  Pair p({}, vo, [&](std::span<const uint8_t> token) {
    seen.assign(token.begin(), token.end());
    return false;
  });
  ASSERT_TRUE(WaitUntil([&] { return p.viewer->state() == LinkState::kDisconnected; }));
  EXPECT_EQ(p.viewer->error(), LinkError::kRejected);
  EXPECT_EQ(p.host->error(), LinkError::kRejected);
  EXPECT_EQ(seen, vo.auth_token);
}

TEST(Link, AuthHookAcceptsTheRightToken) {
  LinkOptions vo;
  vo.auth_token = {1, 2, 3};
  Pair p({}, vo, [](std::span<const uint8_t> t) { return t.size() == 3 && t[2] == 3; });
  ASSERT_TRUE(WaitUntil([&] { return p.viewer->state() == LinkState::kConnected; }));
}

TEST(Link, ViewerRefusesAHostThatNeedsANewerViewer) {
  auto [a, b] = CreateLocalPipe();
  RawPeer host(std::move(a));
  Events ev;
  Link viewer(Link::Role::kViewer, b.get(), {}, ev.Handlers());
  b->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) { viewer.OnReceive(ch, bytes); });
  viewer.Start();
  ASSERT_TRUE(WaitUntil([&] { return host.Find<proto::ViewerHello>().has_value(); }));
  proto::Welcome w;
  w.version = 9;
  w.min_version = 9;
  host.Send(w);
  ASSERT_TRUE(WaitUntil([&] { return viewer.disconnected(); }));
  EXPECT_EQ(viewer.error(), LinkError::kProtocol);
  ASSERT_TRUE(WaitUntil([&] { return host.Find<proto::Goodbye>().has_value(); }));
  EXPECT_EQ(host.Find<proto::Goodbye>()->reason, proto::GoodbyeReason::kIncompatible);
  viewer.Shutdown();
  b.reset();
}

TEST(Link, ViewerAcceptsANewerHostThatStillServesIt) {
  auto [a, b] = CreateLocalPipe();
  RawPeer host(std::move(a));
  Events ev;
  Link viewer(Link::Role::kViewer, b.get(), {}, ev.Handlers());
  b->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) { viewer.OnReceive(ch, bytes); });
  viewer.Start();
  proto::Welcome w;
  w.version = 9;  // newer, but serves version 4 viewers
  w.min_version = 4;
  host.Send(w);
  ASSERT_TRUE(WaitUntil([&] { return viewer.state() == LinkState::kConnected; }));
  viewer.Shutdown();
  b.reset();
}

TEST(Link, HostRefusesAnOldViewer) {
  auto [a, b] = CreateLocalPipe();
  RawPeer viewer(std::move(b));
  Events ev;
  Link host(Link::Role::kHost, a.get(), {}, ev.Handlers());
  a->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) { host.OnReceive(ch, bytes); });
  host.Start();
  proto::ViewerHello vh;
  vh.version = 3;
  vh.min_version = 3;
  viewer.Send(vh);
  ASSERT_TRUE(WaitUntil([&] { return host.disconnected(); }));
  EXPECT_EQ(host.error(), LinkError::kProtocol);
  ASSERT_TRUE(WaitUntil([&] { return viewer.Find<proto::Goodbye>().has_value(); }));
  EXPECT_FALSE(viewer.Find<proto::Welcome>().has_value());
  host.Shutdown();
  a.reset();
}

TEST(Link, ConnectedHostTimesOutWhenTheViewerGoesQuiet) {
  auto [a, b] = CreateLocalPipe();
  RawPeer viewer(std::move(b));  // says hello, then nothing
  Events ev;
  LinkOptions o;
  o.peer_timeout_ms = 200;
  Link host(Link::Role::kHost, a.get(), o, ev.Handlers());
  a->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) { host.OnReceive(ch, bytes); });
  host.Start();
  viewer.Send(proto::ViewerHello{});
  ASSERT_TRUE(WaitUntil([&] { return host.state() == LinkState::kConnected; }));
  EXPECT_TRUE(viewer.Find<proto::Welcome>().has_value());
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_TRUE(WaitUntil([&] { return host.disconnected(); }));
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_EQ(host.error(), LinkError::kTimeout);
  EXPECT_GE(ms, 100);
  EXPECT_LT(ms, 2000);
  host.Shutdown();
  a.reset();
}

TEST(Link, ViewerTimesOutWhenNoHostAnswers) {
  auto [a, b] = CreateLocalPipe();
  RawPeer host(std::move(a));
  Events ev;
  LinkOptions o;
  o.peer_timeout_ms = 150;
  Link viewer(Link::Role::kViewer, b.get(), o, ev.Handlers());
  b->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) { viewer.OnReceive(ch, bytes); });
  viewer.Start();
  ASSERT_TRUE(WaitUntil([&] { return viewer.disconnected(); }));
  EXPECT_EQ(viewer.error(), LinkError::kTimeout);
  viewer.Shutdown();
  b.reset();
}

TEST(Link, PingsKeepAnIdleConnectionAlive) {
  LinkOptions o;
  o.peer_timeout_ms = 200;
  Pair p(o, o);
  ASSERT_TRUE(WaitUntil([&] { return p.viewer->state() == LinkState::kConnected; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  EXPECT_EQ(p.host->state(), LinkState::kConnected);
  EXPECT_EQ(p.viewer->state(), LinkState::kConnected);
}

TEST(Link, HandlersMayCallBackIntoTheLink) {
  // A state handler that sends a message (and one that sends after the
  // disconnect) must not deadlock.
  auto [a, b] = CreateLocalPipe();
  Events host_ev;
  Link* viewer_ptr = nullptr;
  std::atomic<int> calls{0};
  Link::Handlers vh;
  vh.on_state = [&](LinkState s, LinkError) {
    std::string err;
    viewer_ptr->SendMessage(kFirstUserMessage, {}, &err);
    if (s == LinkState::kConnected) viewer_ptr->Close();
    ++calls;
  };
  Link host(Link::Role::kHost, a.get(), {}, host_ev.Handlers());
  Link viewer(Link::Role::kViewer, b.get(), {}, std::move(vh));
  viewer_ptr = &viewer;
  a->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) { host.OnReceive(ch, bytes); });
  b->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) { viewer.OnReceive(ch, bytes); });
  host.Start();
  viewer.Start();
  ASSERT_TRUE(WaitUntil([&] { return calls == 2; }));
  ASSERT_TRUE(WaitUntil([&] { return host.disconnected(); }));
  viewer.Shutdown();
  host.Shutdown();
  a.reset();
  b.reset();
}
