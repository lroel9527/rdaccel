#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "transport/tcp_transport.h"

using namespace rdacore::transport;

TEST(TcpTransport, LoopbackRoundTrip) {
  constexpr uint16_t kPort = 47391;
  std::unique_ptr<TcpTransport> server;
  std::string err;
  std::thread accept_thread([&] { server = TcpTransport::Listen(kPort, &err); });

  std::unique_ptr<TcpTransport> client;
  for (int i = 0; i < 50 && !client; ++i) {
    client = TcpTransport::Connect("127.0.0.1", kPort, nullptr);
    if (!client) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  accept_thread.join();
  ASSERT_TRUE(client);
  ASSERT_TRUE(server) << err;

  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::pair<Channel, std::vector<uint8_t>>> got;
  server->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(mu);
    got.emplace_back(ch, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    cv.notify_all();
  });

  std::vector<uint8_t> big(1 << 20);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 31);
  const uint8_t small = 7;
  ASSERT_TRUE(client->Send(Channel::kVideo, big));
  ASSERT_TRUE(client->Send(Channel::kInput, {&small, 1}));

  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got.size() == 2; }));
  EXPECT_EQ(got[0].first, Channel::kVideo);
  EXPECT_EQ(got[0].second, big);
  EXPECT_EQ(got[1].first, Channel::kInput);
  EXPECT_EQ(got[1].second, std::vector<uint8_t>{7});
}

TEST(TcpTransport, BindReturnsAtOnceAndFlushesQueuedSendsOnAccept) {
  constexpr uint16_t kPort = 47392;
  std::string err;
  auto server = TcpTransport::Bind(kPort, &err);
  ASSERT_TRUE(server) << err;
  EXPECT_FALSE(server->PeerKnown());
  // Sent before anyone connected: queued, delivered first and in order.
  const uint8_t a = 1, b = 2, c = 3;
  ASSERT_TRUE(server->Send(Channel::kControl, {&a, 1}));
  ASSERT_TRUE(server->Send(Channel::kVideo, {&b, 1}));

  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::pair<Channel, uint8_t>> got;
  auto client = TcpTransport::Connect("::1", kPort, nullptr);
  if (!client) client = TcpTransport::Connect("127.0.0.1", kPort, &err);  // no IPv6 here
  ASSERT_TRUE(client) << err;
  client->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(mu);
    got.emplace_back(ch, bytes[0]);
    cv.notify_all();
  });
  for (int i = 0; i < 200 && !server->PeerKnown(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(server->PeerKnown());
  ASSERT_TRUE(server->Send(Channel::kCursor, {&c, 1}));

  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got.size() == 3; }));
  EXPECT_EQ(got[0], std::make_pair(Channel::kControl, a));
  EXPECT_EQ(got[1], std::make_pair(Channel::kVideo, b));
  EXPECT_EQ(got[2], std::make_pair(Channel::kCursor, c));
}

TEST(TcpTransport, BindAcceptsIPv4AndClosesWithoutAPeer) {
  std::string err;
  {
    auto server = TcpTransport::Bind(47393, &err);
    ASSERT_TRUE(server) << err;
    auto client = TcpTransport::Connect("127.0.0.1", 47393, &err);
    ASSERT_TRUE(client) << err;
    for (int i = 0; i < 200 && !server->PeerKnown(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(server->PeerKnown());
  }
  // Destroyed while still waiting: returns promptly, the port is free again.
  const auto t0 = std::chrono::steady_clock::now();
  { auto server = TcpTransport::Bind(47394, &err); ASSERT_TRUE(server) << err; }
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
  auto again = TcpTransport::Bind(47394, &err);
  EXPECT_TRUE(again) << err;
}
