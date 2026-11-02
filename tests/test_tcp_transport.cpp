#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "transport/tcp_transport.h"

using namespace rdaccel::transport;

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
