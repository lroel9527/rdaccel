#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <string>
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
  // far, so the target may stay above it, but not above twice it: it comes
  // down by 10% a step.
  st.queued_bytes = 1'000;
  auto d = rc.Update(0, st);
  EXPECT_FALSE(d.skip_frame);
  EXPECT_EQ(rc.target_bps(), 7'200'000);
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

TEST(RateController, TargetComesDownToTwiceTheEstimate) {
  // The path fell from 4 to 1 Mbps while the encoder sent less than either:
  // no queue, so no back-off, but the target must not stay at 8 Mbps (the
  // Mac kept it there for 7 s), or the next busy scene floods the path.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 4'000'000;
  st.rtt_us = 40'000;
  rc.Update(0, st);
  EXPECT_EQ(rc.target_bps(), 8'000'000);
  st.bandwidth_bps = 1'000'000;
  for (uint64_t t = 250'000; t <= 4'000'000; t += 250'000) rc.Update(t, st);
  EXPECT_EQ(rc.target_bps(), 2'000'000);
}

TEST(RateController, BacksOffWhenTheRoundTripGrows) {
  // The local queue stays short, but the round trip has tripled: a queue is
  // building inside the network, so the target drops below the estimate.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 4'000'000;
  st.queued_bytes = 1'000;
  st.rtt_us = 40'000;
  rc.Update(0, st);
  EXPECT_EQ(rc.target_bps(), 8'000'000);
  st.rtt_us = 60'000;  // a little jitter is fine
  rc.Update(300'000, st);
  EXPECT_EQ(rc.target_bps(), 8'000'000);
  st.rtt_us = 200'000;
  auto d = rc.Update(600'000, st);
  EXPECT_EQ(d.new_target_bps, 3'400'000);
}

TEST(RateController, AsksLessOfAnEncoderThatOvershoots) {
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 1'000'000;
  st.queued_bytes = 10'000;  // 80 ms: congested
  uint64_t sent = 1;
  auto d = rc.Update(0, st, sent);
  EXPECT_EQ(d.new_target_bps, 850'000);
  // The queue stays long but drains slowly, so the target holds, while the
  // encoder makes 1.5x what it is asked for.
  int64_t asked = rc.encoder_bps();
  uint64_t t = 0;
  for (int i = 0; i < 12; ++i) {
    t += 250'000;
    st.queued_bytes -= 10;
    sent += static_cast<uint64_t>(static_cast<double>(asked) * 1.5 / 8 / 4);
    d = rc.Update(t, st, sent);
    if (d.new_target_bps) asked = d.new_target_bps;
  }
  EXPECT_EQ(rc.target_bps(), 850'000);
  EXPECT_LT(rc.encoder_bps(), 850'000 * 3 / 4);
  // Now it keeps to the rate: the correction eases off again.
  for (int i = 0; i < 40; ++i) {
    t += 250'000;
    st.queued_bytes -= 10;
    sent += static_cast<uint64_t>(asked / 8 / 4);
    d = rc.Update(t, st, sent);
    if (d.new_target_bps) asked = d.new_target_bps;
  }
  EXPECT_GT(rc.encoder_bps(), 850'000 * 9 / 10);
}

TEST(RateController, OneBigPictureIsNotAnOvershoot) {
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 1'000'000;
  st.queued_bytes = 10'000;
  uint64_t sent = 1;
  rc.Update(0, st, sent);
  const int64_t asked = rc.encoder_bps();
  uint64_t t = 0;
  for (int i = 0; i < 16; ++i) {
    t += 250'000;
    st.queued_bytes -= 10;  // draining slowly: the target holds
    // A keyframe of 3 s worth in the first second, then on target.
    sent += static_cast<uint64_t>(asked / 8 / 4) + (i == 0 ? static_cast<uint64_t>(asked * 3 / 8) : 0);
    rc.Update(t, st, sent);
  }
  EXPECT_EQ(rc.encoder_bps(), asked);
}

TEST(RateController, KeepsBackingOffWhileTheQueueDoesNotDrain) {
  // 5% steps used to be below the reconfiguration threshold and were lost.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 1'000'000;
  st.queued_bytes = 10'000;
  rc.Update(0, st);
  EXPECT_EQ(rc.target_bps(), 850'000);
  for (uint64_t t = 250'000; t <= 1'000'000; t += 250'000) rc.Update(t, st);
  EXPECT_LT(rc.target_bps(), 850'000 * 85 / 100);
  EXPECT_LT(rc.encoder_bps(), 850'000 * 9 / 10);
}

TEST(ReliableTransport, EstimateFollowsABandwidthDrop) {
  // 4 Mbps, then 1 Mbps from 2 s on, with a sender that keeps the queue
  // short (as the Host does by skipping frames): the estimate has to come
  // down to the new rate, or the Host keeps encoding for the old one.
  net::LinkConditions c;
  c.bandwidth_bps = 4'000'000;
  c.delay_ms = 20;
  c.queue_ms = 200;
  c.bandwidth_schedule = {{2000, 1'000'000}};
  Sink sink;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = 20});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(sink.Fn());
  const auto chunk = Pattern(10'000, 4);
  const uint64_t t0 = capture::MonotonicMicros();
  TransportStats st;
  uint64_t caught_up = 0;  // when the estimate first came down after the drop
  size_t got_at_3s = 0;    // bytes delivered by 3 s, to measure the last 2 s
  std::string trace;       // the estimate and round trip every 250 ms
  uint64_t next_trace = 0;
  while (capture::MonotonicMicros() - t0 < 5'000'000) {
    host.GetStats(&st);
    const uint64_t t = capture::MonotonicMicros() - t0;
    if (t >= next_trace) {
      next_trace += 250'000;
      trace += std::to_string(st.bandwidth_bps / 1000) + "k/" + std::to_string(st.rtt_us / 1000) + "ms ";
    }
    if (!caught_up && t > 2'000'000 && st.bandwidth_bps > 0 && st.bandwidth_bps < 1'300'000) caught_up = t;
    if (t >= 3'000'000 && got_at_3s == 0) {
      std::lock_guard<std::mutex> lock(sink.mu);
      got_at_3s = sink.got[static_cast<int>(Channel::kVideo)].size() + 1;
    }
    if (st.queued_bytes < 20'000) host.Send(Channel::kVideo, chunk);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::printf("estimate caught up %.2f s after the drop, ends at %lld kbps\n",
              caught_up ? (caught_up - 2'000'000) / 1e6 : -1.0, static_cast<long long>(st.bandwidth_bps / 1000));
  std::printf("estimate/rtt every 250 ms: %s\n", trace.c_str());
  // Before the time window it took ~2.7 s after the drop; now ~0.4 s here
  // and ~1.3 s on a macOS laptop (coarser timers).
  ASSERT_NE(caught_up, 0u);
  EXPECT_LT(caught_up, 3'800'000u);
  host.GetStats(&st);
  EXPECT_LT(st.bandwidth_bps, 1'300'000);
  // Not far below what the path delivered meanwhile. (Where the runner
  // starves the sending thread, as macOS CI does, less goes through: the
  // estimate is judged against that, not against the 1 Mbps.)
  int64_t delivered_bps;
  {
    std::lock_guard<std::mutex> lock(sink.mu);
    delivered_bps = static_cast<int64_t>(sink.got[static_cast<int>(Channel::kVideo)].size() - got_at_3s) * 8 / 2;
  }
  std::printf("delivered %lld kbps in the last 2 s\n", static_cast<long long>(delivered_bps / 1000));
  EXPECT_GT(st.bandwidth_bps, std::min<int64_t>(700'000, delivered_bps * 7 / 10));
  EXPECT_GT(st.bandwidth_bps, 300'000);
}

TEST(ReliableTransport, EstimateFollowsADropBelowWhatAnAppLimitedSenderSends) {
  // The sender keeps to ~640 kbps (an encoder at its own rate, not filling
  // the 4 Mbps path); the path drops to 400 kbps at 1.5 s. Its samples are
  // application limited, but the queue that builds shows the path's rate:
  // the estimate must come down within a couple of seconds (on the Mac it
  // stayed at 4 Mbps for 3-4 s while the round trip passed 240 ms).
  net::LinkConditions c;
  c.bandwidth_bps = 4'000'000;
  c.delay_ms = 20;
  c.queue_ms = 2000;
  c.bandwidth_schedule = {{1500, 400'000}};
  Sink sink;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = 20});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(sink.Fn());
  const auto chunk = Pattern(2'400, 5);  // every 30 ms: 640 kbps
  const uint64_t t0 = capture::MonotonicMicros();
  TransportStats st;
  uint64_t caught_up = 0;
  std::string trace;
  uint64_t next_trace = 0;
  for (uint64_t next = t0; capture::MonotonicMicros() - t0 < 5'000'000; next += 30'000) {
    host.GetStats(&st);
    const uint64_t t = capture::MonotonicMicros() - t0;
    if (t >= next_trace) {
      next_trace += 250'000;
      trace += std::to_string(st.bandwidth_bps / 1000) + "k/" + std::to_string(st.rtt_us / 1000) + "ms ";
    }
    if (!caught_up && t > 1'500'000 && st.bandwidth_bps > 0 && st.bandwidth_bps < 600'000) caught_up = t;
    host.Send(Channel::kVideo, chunk);
    const uint64_t now = capture::MonotonicMicros();
    if (next + 30'000 > now) std::this_thread::sleep_for(std::chrono::microseconds(next + 30'000 - now));
  }
  std::printf("app-limited: estimate below 600 kbps %.2f s after the drop\nestimate/rtt every 250 ms: %s\n",
              caught_up ? (caught_up - 1'500'000) / 1e6 : -1.0, trace.c_str());
  // ~0.25 s here. Before: 1.9 s with the application-limited samples left
  // out, and never while a probe timeout declared everything lost.
  ASSERT_NE(caught_up, 0u);
  EXPECT_LT(caught_up, 1'500'000u + 1'500'000u);
}

TEST(ReliableTransport, CursorStaysFastUnderASaturatingSender) {
  // Video keeps the 1.5 Mbps path busy (as VideoToolbox does on the Mac);
  // small cursor messages every 16 ms must not wait behind a standing queue
  // inside the network: the sender's own queue lets them jump ahead.
  net::LinkConditions c;
  c.bandwidth_bps = 1'500'000;
  c.delay_ms = 20;
  c.queue_ms = 200;
  std::mutex mu;
  std::vector<double> lat_ms;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = 20});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  std::atomic<uint64_t> video_bytes{0};
  viewer.SetReceiver([&](transport::Channel ch, std::span<const uint8_t> bytes) {
    if (ch == transport::Channel::kVideo) video_bytes += bytes.size();
    if (ch != transport::Channel::kCursor) return;
    const uint64_t now = capture::MonotonicMicros();
    for (size_t i = 0; i + 8 <= bytes.size(); i += 8) {
      uint64_t sent;
      std::memcpy(&sent, bytes.data() + i, 8);
      std::lock_guard<std::mutex> lock(mu);
      lat_ms.push_back((now - sent) / 1000.0);
    }
  });
  const auto chunk = Pattern(6'000, 5);
  const uint64_t t0 = capture::MonotonicMicros();
  uint64_t next_cursor = t0;
  TransportStats st;
  while (capture::MonotonicMicros() - t0 < 6'000'000) {
    const uint64_t now = capture::MonotonicMicros();
    host.GetStats(&st);
    if (st.queued_bytes < 30'000) host.Send(Channel::kVideo, chunk);
    if (now >= next_cursor) {
      uint8_t msg[8];
      std::memcpy(msg, &now, 8);
      host.Send(Channel::kCursor, msg);
      next_cursor += 16'000;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::lock_guard<std::mutex> lock(mu);
  // Past the start-up phase.
  std::vector<double> v(lat_ms.begin() + std::min<size_t>(lat_ms.size(), 120), lat_ms.end());
  ASSERT_GT(v.size(), 100u);
  std::sort(v.begin(), v.end());
  const double p50 = v[v.size() / 2], p95 = v[v.size() * 95 / 100];
  const double kbps = static_cast<double>(video_bytes.load()) * 8 / 6.0 / 1000;
  std::printf("cursor under load: p50 %.1f p95 %.1f ms, video %.0f kbps, rtt %u ms\n", p50, p95, kbps,
              static_cast<unsigned>(st.rtt_us / 1000));
  EXPECT_LT(p50, 50);  // 68 ms here before the sender drained the queue (more on loaded CI Macs)
  EXPECT_LT(p95, 100);
  // Without giving up the path's capacity.
  EXPECT_GT(kbps, 1'200);
}
