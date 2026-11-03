// The encrypted transport (transport/secure_transport.h) over an in-process
// pipe: round trips on every channel, and every way it must refuse.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "monocypher.h"
#include "rdacore/transport.h"
#include "transport/secure_transport.h"

using namespace rdacore;
using transport::SecureTransport;

namespace {

bool WaitUntil(const std::function<bool()>& done, int ms = 5000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

std::array<uint8_t, 32> Key(uint8_t seed) {
  std::array<uint8_t, 32> k;
  for (size_t i = 0; i < k.size(); ++i) k[i] = static_cast<uint8_t>(seed + i * 7);
  return k;
}

struct Sink {
  std::mutex mu;
  std::array<std::vector<uint8_t>, kChannelCount> got;
  std::atomic<int> errors{0};
  ConnectionError last_error = ConnectionError::kNone;
  Transport::ReceiveFn Fn() {
    return [this](Channel ch, std::span<const uint8_t> b) {
      std::lock_guard<std::mutex> lock(mu);
      auto& v = got[static_cast<size_t>(ch)];
      v.insert(v.end(), b.begin(), b.end());
    };
  }
  SecureTransport::ErrorFn OnError() {
    return [this](ConnectionError e, const std::string&) {
      std::lock_guard<std::mutex> lock(mu);
      last_error = e;
      ++errors;
    };
  }
  size_t size(Channel ch) {
    std::lock_guard<std::mutex> lock(mu);
    return got[static_cast<size_t>(ch)].size();
  }
};

// Sits between the pipe and a SecureTransport and lets a test change bytes.
class Tap final : public Transport {
 public:
  explicit Tap(std::unique_ptr<Transport> inner) : inner_(std::move(inner)) {}
  ~Tap() override { inner_.reset(); }
  std::function<void(Channel, std::vector<uint8_t>&)> on_send;
  std::atomic<bool> duplicate_next{false};
  bool Send(Channel ch, std::span<const uint8_t> b) override {
    std::vector<uint8_t> v(b.begin(), b.end());
    if (on_send) on_send(ch, v);
    if (duplicate_next.exchange(false)) inner_->Send(ch, v);
    return inner_->Send(ch, v);
  }
  void SetReceiver(ReceiveFn fn) override { inner_->SetReceiver(std::move(fn)); }
  void Close() override { inner_->Close(); }

 private:
  std::unique_ptr<Transport> inner_;
};

}  // namespace

TEST(SecureTransport, CipherKnownAnswer) {
  // XChaCha20-Poly1305, computed independently with libsodium.
  uint8_t key[32], nonce[24];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
  for (int i = 0; i < 24; ++i) nonce[i] = static_cast<uint8_t>(0x40 + i);
  const char pt[] = "rdaccel known answer test";
  const uint8_t ad = 3;
  const uint8_t want[41] = {0xa6, 0x5d, 0x64, 0x13, 0xb3, 0x85, 0x15, 0x36, 0xe4, 0x9a, 0xe8, 0xc9, 0xc1, 0xbc,
                            0x04, 0xfc, 0xe1, 0xcd, 0xc8, 0xb6, 0x33, 0x2d, 0x36, 0xe9, 0x1e, 0x1d, 0x86, 0xd0,
                            0x1b, 0xe3, 0x6a, 0xac, 0xdd, 0x43, 0xf5, 0x74, 0x8b, 0x17, 0x9d, 0xe6, 0x54};
  uint8_t ct[25], mac[16];
  crypto_aead_lock(ct, mac, key, nonce, &ad, 1, reinterpret_cast<const uint8_t*>(pt), 25);
  EXPECT_EQ(std::memcmp(ct, want, 25), 0);
  EXPECT_EQ(std::memcmp(mac, want + 25, 16), 0);
}

TEST(SecureTransport, RoundTripOnEveryChannel) {
  auto [a, b] = CreateLocalPipe();
  const auto k = Key(1);
  SecureTransport host(std::move(a), true, k);
  SecureTransport viewer(std::move(b), false, k);
  Sink hs, vs;
  host.SetErrorHandler(hs.OnError());
  viewer.SetErrorHandler(vs.OnError());
  // Sent before the keys exist: held, then delivered in order.
  std::vector<uint8_t> big(300'000);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 13);
  ASSERT_TRUE(host.Send(Channel::kVideo, big));
  EXPECT_FALSE(host.PeerKnown());
  host.SetReceiver(hs.Fn());
  viewer.SetReceiver(vs.Fn());
  ASSERT_TRUE(WaitUntil([&] { return host.PeerKnown() && viewer.PeerKnown(); }));
  for (int c = 0; c < kChannelCount; ++c) {
    const std::vector<uint8_t> msg(100 + c, static_cast<uint8_t>(c));
    ASSERT_TRUE(viewer.Send(static_cast<Channel>(c), msg));
    ASSERT_TRUE(host.Send(static_cast<Channel>(c), msg));
  }
  ASSERT_TRUE(WaitUntil([&] { return vs.size(Channel::kVideo) == big.size() + 103; }));
  ASSERT_TRUE(WaitUntil([&] { return hs.size(Channel::kMessage) == 105; }));
  std::lock_guard<std::mutex> lock(vs.mu);
  EXPECT_TRUE(std::equal(big.begin(), big.end(), vs.got[3].begin()));
  EXPECT_EQ(vs.got[0].size(), 100u);
  EXPECT_EQ(hs.errors.load(), 0);
  EXPECT_EQ(vs.errors.load(), 0);
}

TEST(SecureTransport, CiphertextDoesNotContainThePlaintext) {
  auto [a, b] = CreateLocalPipe();
  auto tap = std::make_unique<Tap>(std::move(a));
  std::mutex mu;
  std::vector<uint8_t> wire;
  tap->on_send = [&](Channel, std::vector<uint8_t>& v) {
    std::lock_guard<std::mutex> lock(mu);
    wire.insert(wire.end(), v.begin(), v.end());
  };
  const auto k = Key(2);
  SecureTransport host(std::move(tap), true, k);
  SecureTransport viewer(std::move(b), false, k);
  Sink hs, vs;
  host.SetReceiver(hs.Fn());
  viewer.SetReceiver(vs.Fn());
  const std::string secret = "the clipboard holds a password";
  host.Send(Channel::kMessage, {reinterpret_cast<const uint8_t*>(secret.data()), secret.size()});
  ASSERT_TRUE(WaitUntil([&] { return vs.size(Channel::kMessage) == secret.size(); }));
  std::lock_guard<std::mutex> lock(mu);
  EXPECT_EQ(std::search(wire.begin(), wire.end(), secret.begin(), secret.end()), wire.end());
}

TEST(SecureTransport, WrongKeyIsRejected) {
  auto [a, b] = CreateLocalPipe();
  SecureTransport host(std::move(a), true, Key(3));
  SecureTransport viewer(std::move(b), false, Key(4));
  Sink hs, vs;
  host.SetErrorHandler(hs.OnError());
  viewer.SetErrorHandler(vs.OnError());
  host.SetReceiver(hs.Fn());
  viewer.SetReceiver(vs.Fn());
  ASSERT_TRUE(WaitUntil([&] { return host.PeerKnown() && viewer.PeerKnown(); }));
  host.Send(Channel::kControl, std::vector<uint8_t>(10, 1));
  viewer.Send(Channel::kControl, std::vector<uint8_t>(10, 1));
  ASSERT_TRUE(WaitUntil([&] { return hs.errors == 1 && vs.errors == 1; }));
  EXPECT_EQ(vs.last_error, ConnectionError::kRejected);
  EXPECT_EQ(vs.size(Channel::kControl), 0u);
}

TEST(SecureTransport, PlaintextPeerIsRefused) {
  auto [a, b] = CreateLocalPipe();
  SecureTransport host(std::move(a), true, Key(5));
  Sink hs;
  host.SetErrorHandler(hs.OnError());
  host.SetReceiver(hs.Fn());
  b->SetReceiver([](Channel, std::span<const uint8_t>) {});
  const uint8_t viewer_hello[] = {12, 4, 4, 3, 1, 0};  // an unencrypted ViewerHello
  b->Send(Channel::kControl, viewer_hello);
  ASSERT_TRUE(WaitUntil([&] { return hs.errors == 1; }));
  EXPECT_EQ(hs.last_error, ConnectionError::kProtocol);
  b.reset();
}

TEST(SecureTransport, TamperingAndReplayAreDetected) {
  for (int mode = 0; mode < 2; ++mode) {
    auto [a, b] = CreateLocalPipe();
    auto tap = std::make_unique<Tap>(std::move(a));
    Tap* t = tap.get();
    const auto k = Key(6);
    SecureTransport host(std::move(tap), true, k);
    SecureTransport viewer(std::move(b), false, k);
    Sink hs, vs;
    viewer.SetErrorHandler(vs.OnError());
    host.SetReceiver(hs.Fn());
    viewer.SetReceiver(vs.Fn());
    ASSERT_TRUE(WaitUntil([&] { return host.PeerKnown(); }));
    host.Send(Channel::kInput, std::vector<uint8_t>(50, 9));
    ASSERT_TRUE(WaitUntil([&] { return vs.size(Channel::kInput) == 50; }));
    if (mode == 0) {
      t->on_send = [](Channel, std::vector<uint8_t>& v) { v[10] ^= 0x01; };  // flip one bit
    } else {
      t->duplicate_next = true;  // send the same record twice
    }
    host.Send(Channel::kInput, std::vector<uint8_t>(50, 9));
    ASSERT_TRUE(WaitUntil([&] { return vs.errors == 1; })) << mode;
    EXPECT_EQ(vs.last_error, ConnectionError::kRejected);
    EXPECT_EQ(vs.size(Channel::kInput), mode == 0 ? 50u : 100u);
  }
}
