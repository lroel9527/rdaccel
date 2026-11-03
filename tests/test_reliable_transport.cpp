#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "capture/capturer.h"
#include "net/reliable_transport.h"

using namespace rdacore;

namespace {

struct Sink {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> got[5];

  transport::ITransport::ReceiveFn Fn() {
    return [this](transport::Channel ch, std::span<const uint8_t> b) {
      std::lock_guard<std::mutex> lock(mu);
      auto& v = got[static_cast<int>(ch)];
      v.insert(v.end(), b.begin(), b.end());
      cv.notify_all();
    };
  }
  bool WaitFor(int ch, size_t n, int seconds = 20) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return got[ch].size() >= n; });
  }
};

std::vector<uint8_t> Pattern(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  for (auto& b : v) {
    seed = seed * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(seed >> 24);
  }
  return v;
}

}  // namespace

TEST(ReliableTransport, DeliversEverythingInOrderDespiteLossAndReordering) {
  net::LinkConditions c;
  c.bandwidth_bps = 20'000'000;
  c.delay_ms = 10;
  c.jitter_ms = 4;
  c.loss = 0.05;
  Sink at_viewer, at_host;  // outlive the transports that call into them
  auto [a, b] = net::MakeSimulatedLink(c, c);
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(at_viewer.Fn());
  host.SetReceiver(at_host.Fn());

  const auto video = Pattern(600'000, 1);
  const auto control = Pattern(3'000, 2);
  // 60 video chunks of 10 kB with a 300-byte control message after every 6th.
  for (size_t i = 0; i < 60; ++i) {
    host.Send(Channel::kVideo, std::span(video).subspan(i * 10'000, 10'000));
    if (i % 6 == 5) host.Send(Channel::kControl, std::span(control).subspan(i / 6 * 300, 300));
  }
  const auto input = Pattern(5'000, 3);
  for (size_t i = 0; i < input.size(); i += 50) viewer.Send(Channel::kInput, std::span(input).subspan(i, 50));

  ASSERT_TRUE(at_viewer.WaitFor(3, video.size()));
  ASSERT_TRUE(at_viewer.WaitFor(0, 3'000));
  ASSERT_TRUE(at_host.WaitFor(1, input.size()));
  EXPECT_EQ(at_viewer.got[3], video);
  EXPECT_EQ(at_viewer.got[0], control);
  EXPECT_EQ(at_host.got[1], input);
  TransportStats st;
  ASSERT_TRUE(host.GetStats(&st));
  EXPECT_GT(st.packets_lost, 0u);
  EXPECT_GT(st.rtt_us, 15'000u);
}

TEST(ReliableTransport, EstimatesBandwidthWithoutFillingTheQueue) {
  net::LinkConditions c;
  c.bandwidth_bps = 4'000'000;
  c.delay_ms = 20;
  c.queue_ms = 500;
  Sink sink;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = 20});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(sink.Fn());
  // Keep the sender saturated for 3 seconds.
  const auto chunk = Pattern(10'000, 4);
  size_t sent = 0;
  const uint64_t t0 = capture::MonotonicMicros();
  TransportStats st;
  while (capture::MonotonicMicros() - t0 < 3'000'000) {
    host.GetStats(&st);
    if (st.queued_bytes < 100'000) {
      host.Send(Channel::kVideo, chunk);
      sent += chunk.size();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  host.GetStats(&st);
  EXPECT_GT(st.bandwidth_bps, 3'200'000);
  EXPECT_LT(st.bandwidth_bps, 5'000'000);
  // Base RTT is 40 ms; a loss-based sender would add the 500 ms buffer.
  EXPECT_LT(st.rtt_us, 120'000u);
  // Throughput close to the link rate.
  ASSERT_TRUE(sink.WaitFor(3, 1));
  std::lock_guard<std::mutex> lock(sink.mu);
  EXPECT_GT(sink.got[3].size() * 8.0 / 3.0, 3'000'000.0);
}

TEST(ReliableTransport, UdpLoopback) {
  Sink at_client, at_server;
  std::unique_ptr<net::DatagramSocket> server_sock;
  std::string err;
  std::thread listener([&] { server_sock = net::ListenUdpSocket(47811, &err); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto client_sock = net::ConnectUdpSocket("127.0.0.1", 47811, &err);
  ASSERT_TRUE(client_sock) << err;
  net::ReliableTransport client(std::move(client_sock), {.client = true});
  listener.join();
  ASSERT_TRUE(server_sock) << err;
  net::ReliableTransport server(std::move(server_sock));
  client.SetReceiver(at_client.Fn());
  server.SetReceiver(at_server.Fn());
  const auto data = Pattern(300'000, 5);
  server.Send(Channel::kVideo, data);
  client.Send(Channel::kInput, std::span(data).subspan(0, 100));
  ASSERT_TRUE(at_client.WaitFor(3, data.size()));
  ASSERT_TRUE(at_server.WaitFor(1, 100));
  EXPECT_EQ(at_client.got[3], data);
}

#include "session/rate_control.h"

TEST(RateController, SkipsFramesAndBacksOffWhenTheQueueBuilds) {
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 2'000'000;
  // Short queue: no skipping. The estimate only bounds what was sent so
  // far, so it does not lower the target.
  st.queued_bytes = 1'000;
  auto d = rc.Update(0, st);
  EXPECT_FALSE(d.skip_frame);
  EXPECT_EQ(rc.target_bps(), 8'000'000);
  // 100 ms of queue at 2 Mbps: skip, and drop to 85% of the estimate.
  st.queued_bytes = 25'000;
  d = rc.Update(300'000, st);
  EXPECT_TRUE(d.skip_frame);
  EXPECT_EQ(d.new_target_bps, 1'700'000);
  // Still congested but draining: hold.
  st.queued_bytes = 20'000;
  d = rc.Update(600'000, st);
  EXPECT_EQ(rc.target_bps(), 1'700'000);
  // Queue gone: grow by a quarter per step, up to twice the estimate.
  st.queued_bytes = 0;
  rc.Update(900'000, st);
  EXPECT_EQ(rc.target_bps(), 2'125'000);
  for (uint64_t t = 1'200'000; t < 5'000'000; t += 300'000) rc.Update(t, st);
  EXPECT_EQ(rc.target_bps(), 4'000'000);
}
