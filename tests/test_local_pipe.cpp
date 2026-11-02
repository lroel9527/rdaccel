#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "transport/local_pipe.h"

using namespace rdaccel::transport;

TEST(LocalPipe, DeliversInOrderInBothDirections) {
  auto [a, b] = MakeLocalPipe();

  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> at_b, at_a;

  b->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) {
    EXPECT_EQ(ch, Channel::kVideo);
    std::lock_guard<std::mutex> lock(mu);
    at_b.insert(at_b.end(), bytes.begin(), bytes.end());
    cv.notify_all();
  });
  a->SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) {
    EXPECT_EQ(ch, Channel::kInput);
    std::lock_guard<std::mutex> lock(mu);
    at_a.insert(at_a.end(), bytes.begin(), bytes.end());
    cv.notify_all();
  });

  for (uint8_t i = 0; i < 100; ++i) ASSERT_TRUE(a->Send(Channel::kVideo, {&i, 1}));
  const uint8_t x = 9;
  ASSERT_TRUE(b->Send(Channel::kInput, {&x, 1}));

  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                          [&] { return at_b.size() == 100 && at_a.size() == 1; }));
  for (uint8_t i = 0; i < 100; ++i) EXPECT_EQ(at_b[i], i);
  EXPECT_EQ(at_a[0], 9);
}

TEST(LocalPipe, BytesSentBeforeReceiverIsSetAreKept) {
  auto [a, b] = MakeLocalPipe();
  const uint8_t v = 1;
  ASSERT_TRUE(a->Send(Channel::kControl, {&v, 1}));

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  b->SetReceiver([&](Channel, std::span<const uint8_t>) {
    std::lock_guard<std::mutex> lock(mu);
    got = true;
    cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(mu);
  EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got; }));
}

TEST(LocalPipe, SendAfterCloseFails) {
  auto [a, b] = MakeLocalPipe();
  a->Close();
  const uint8_t v = 1;
  EXPECT_FALSE(a->Send(Channel::kControl, {&v, 1}));
  EXPECT_FALSE(b->Send(Channel::kControl, {&v, 1}));
}
