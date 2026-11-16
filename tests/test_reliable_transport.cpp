#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
  std::vector<uint8_t> got[kChannelCount];

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
  // Queue gone: grow by a quarter per step, but the queue showed the
  // estimate to be the path's rate, so the ceiling starts there (last
  // congested at 0.6 s) and opens by 10% of it per second.
  st.queued_bytes = 0;
  rc.Update(900'000, st);
  EXPECT_EQ(rc.target_bps(), 2'060'000);
  for (uint64_t t = 1'200'000; t < 5'000'000; t += 300'000) rc.Update(t, st);
  EXPECT_EQ(rc.target_bps(), 2'840'000);
  // Twice the estimate again ten seconds after the queue.
  for (uint64_t t = 5'100'000; t < 11'000'000; t += 300'000) rc.Update(t, st);
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
  // The estimate fell under half of its peak: the target follows at once,
  // below the new estimate (at 4 -> 0.4 Mbps, coming down 10% a step left
  // it far above the path for 0.4 s).
  rc.Update(250'000, st);
  EXPECT_EQ(rc.target_bps(), 850'000);
  // Then it grows again, but stays within twice the estimate.
  for (uint64_t t = 500'000; t <= 4'000'000; t += 250'000) rc.Update(t, st);
  EXPECT_GT(rc.target_bps(), 1'000'000);
  EXPECT_LE(rc.target_bps(), 2'000'000);
}

TEST(RateController, FirstEstimatesRisingIsNotAFall) {
  // Early in a session the estimate climbs from low readings; only a fall
  // from a peak counts.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.rtt_us = 40'000;
  for (int64_t bw : {500'000, 900'000, 2'000'000, 3'500'000}) {
    st.bandwidth_bps = bw;
    rc.Update(static_cast<uint64_t>(bw), st);
  }
  EXPECT_GT(rc.target_bps(), 3'500'000);
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

TEST(RateController, SkipsEveryOtherFrameOnANetworkQueueAtTheFloor) {
  // At its floor the target can go no lower, yet the encoder may send more
  // than the path takes (VideoToolbox at 300 kbps over a 400 kbps link): a
  // queue stands inside the network with the local one empty. Every other
  // frame is skipped then, and only then.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 400'000;
  st.queued_bytes = 2'000;  // 40 ms: down to the floor
  st.rtt_us = 40'000;
  uint64_t t = 0;
  for (int i = 0; i < 100 && rc.target_bps() > 300'000; ++i) rc.Update(t += 300'000, st);
  ASSERT_EQ(rc.target_bps(), 300'000);
  st.queued_bytes = 0;
  int skipped = 0;
  st.rtt_us = 240'000;
  // A queue counts once every reading of the last 250 ms shows it (jitter
  // is not a queue): nothing is skipped before that.
  for (int i = 0; i < 7; ++i) skipped += rc.Update(t += 33'000, st).skip_frame;  // 231 ms
  EXPECT_EQ(skipped, 0);
  for (int i = 0; i < 10; ++i) skipped += rc.Update(t += 33'000, st).skip_frame;
  EXPECT_EQ(skipped, 5);
  EXPECT_EQ(rc.target_bps(), 300'000);
  // Above the floor, the target comes down instead.
  session::RateController above(8'000'000, 300'000, 50);
  st.bandwidth_bps = 4'000'000;
  st.rtt_us = 40'000;
  above.Update(0, st);
  st.rtt_us = 240'000;
  EXPECT_FALSE(above.Update(33'000, st).skip_frame);
}

TEST(RateController, TheFirstTinyEstimatesAreNoEstimate) {
  // A session's first samples come from a handshake and a few bytes: ~1
  // kbps. With the first picture queued behind them that read as minutes of
  // queue, so frames were skipped and the target fell to its floor.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.queued_bytes = 18'000;  // the first picture
  st.rtt_us = 30'000;
  int skipped = 0;
  uint64_t t = 0;
  for (int64_t bw : {1'000, 5'000, 40'000, 120'000}) {
    st.bandwidth_bps = bw;
    skipped += rc.Update(t, st).skip_frame;
    t += 250'000;
  }
  EXPECT_EQ(skipped, 0);
  EXPECT_EQ(rc.target_bps(), 8'000'000);
  // A real estimate: the controller takes it from there (twice it at most).
  st.bandwidth_bps = 1'000'000;
  st.queued_bytes = 0;
  for (int i = 0; i < 30; ++i) rc.Update(t += 250'000, st);
  EXPECT_LE(rc.target_bps(), 2'000'000);
  // After two seconds even a low estimate counts (a really slow path).
  session::RateController slow(8'000'000, 300'000, 50);
  st.bandwidth_bps = 100'000;
  st.queued_bytes = 18'000;
  slow.Update(0, st);
  EXPECT_FALSE(slow.Update(1'000'000, st).skip_frame);
  EXPECT_TRUE(slow.Update(2'100'000, st).skip_frame);
}

TEST(RateController, ATargetNothingUsesDoesNotGrow) {
  // A 1 Mbps path, the estimate right at it, the screen still (the encoder
  // sends ~20 kbps): the target must not climb to twice the path, where the
  // next burst would overfill it.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 1'000'000;
  st.rtt_us = 200'000;
  uint64_t t = 0, sent = 1;
  for (int i = 0; i < 20; ++i) rc.Update(t += 250'000, st, sent += 600);  // down 10% a step to the ceiling
  const int64_t settled = rc.target_bps();
  EXPECT_LE(settled, 2'000'000);
  // A target well above what it sends, and still nothing: no growth at all.
  for (int i = 0; i < 40; ++i) rc.Update(t += 250'000, st, sent += 600);
  EXPECT_LE(rc.target_bps(), settled);
  // An encoder that pushes against a low target (a busy screen at the
  // floor) still gets more.
  session::RateController low(8'000'000, 300'000, 50);
  st.bandwidth_bps = 4'000'000;
  for (int i = 0; i < 12; ++i) low.Update(t += 250'000, st);
  TransportStats q = st;
  q.queued_bytes = 60'000;  // a queue: down to 85% of the estimate
  low.Update(t += 250'000, q);
  const int64_t before = low.target_bps();
  for (int i = 0; i < 8; ++i) low.Update(t += 250'000, st, sent += static_cast<uint64_t>(before / 8 / 4));
  EXPECT_GT(low.target_bps(), before);
}

TEST(RateController, JitterIsNotAQueue) {
  // A jittery Wi-Fi path: base round trip 30 ms, each reading up to 120 ms
  // more. The lowest reading of every quarter second stays near the base,
  // so the target must not be driven to its floor (it was: 300 kbps on a
  // 4 Mbps path in rdaccel_netsim --delay=15 --jitter=60).
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 4'000'000;
  uint32_t seed = 1;
  int skipped = 0;
  for (uint64_t t = 0; t < 10'000'000; t += 33'000) {
    seed = seed * 1103515245 + 12345;
    st.rtt_us = 30'000 + (seed >> 8) % 120'000;
    skipped += rc.Update(t, st).skip_frame;
  }
  EXPECT_EQ(skipped, 0);
  EXPECT_GE(rc.target_bps(), 4'000'000);
}

TEST(RateController, ABaseDelayThatGrewIsNotAQueueForGood) {
  // The path moved (another access point, a VPN): 20 ms, then 150 ms for
  // good. At first that looks like a queue and the target comes down; once
  // the lowest round trip of the last ten seconds is the new one, the target
  // grows back instead of staying at its floor for the rest of the session.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 4'000'000;
  st.rtt_us = 20'000;
  uint64_t t = 0;
  for (; t < 2'000'000; t += 33'000) rc.Update(t, st);
  st.rtt_us = 150'000;
  for (; t < 4'000'000; t += 33'000) rc.Update(t, st);
  EXPECT_LT(rc.target_bps(), 4'000'000);  // backed off: it looked like a queue
  int skipped_late = 0;
  for (; t < 30'000'000; t += 33'000) {
    const bool skip = rc.Update(t, st).skip_frame;
    if (t > 14'000'000) skipped_late += skip;
  }
  EXPECT_EQ(skipped_late, 0);
  EXPECT_GE(rc.target_bps(), 4'000'000);
}

TEST(RateController, KeepsWhatGoesOutWithinTheEstimateAtTheFloor) {
  // An encoder that cannot go below 2 kB a picture (480 kbps at 30 fps)
  // over a 400 kbps path, with the target at its floor: pictures are skipped
  // so that what goes out stays within 90% of the estimate.
  session::RateController rc(8'000'000, 300'000, 50);
  TransportStats st;
  st.bandwidth_bps = 400'000;
  st.queued_bytes = 2'000;  // 40 ms: down to the floor
  st.rtt_us = 40'000;
  uint64_t t = 0;
  for (int i = 0; i < 100 && rc.target_bps() > 300'000; ++i) rc.Update(t += 300'000, st);
  ASSERT_EQ(rc.target_bps(), 300'000);
  st.queued_bytes = 0;
  uint64_t total = 1'000'000, sent = 0;
  const uint64_t start = total;
  int shown = 0;
  for (int i = 0; i < 300; ++i) {  // 10 s
    if (!rc.Update(t += 33'333, st, total, total).skip_frame) {
      total += 2'000;
      ++shown;
    }
  }
  sent = total - start;
  const double kbps = static_cast<double>(sent) * 8 / 10 / 1000;
  EXPECT_LT(kbps, 375);
  EXPECT_GT(kbps, 330);
  EXPECT_GT(shown, 200);
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
  std::vector<int64_t> late_estimates;  // every 250 ms from 3.5 s on
  while (capture::MonotonicMicros() - t0 < 5'000'000) {
    host.GetStats(&st);
    const uint64_t t = capture::MonotonicMicros() - t0;
    if (t >= next_trace) {
      next_trace += 250'000;
      trace += std::to_string(st.bandwidth_bps / 1000) + "k/" + std::to_string(st.rtt_us / 1000) + "ms ";
      if (t >= 3'500'000) late_estimates.push_back(st.bandwidth_bps);
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
  // The estimate over the last 1.5 s, not one reading: it dips for a moment
  // now and then (531 kbps at the very end once on macOS CI, after 782 for a
  // second), while the failure this guards against, an estimate spiralling
  // down, stays low.
  ASSERT_FALSE(late_estimates.empty());
  std::sort(late_estimates.begin(), late_estimates.end());
  const int64_t late = late_estimates[late_estimates.size() / 2];
  std::printf("delivered %lld kbps in the last 2 s, estimate (median of the last 1.5 s) %lld kbps\n",
              static_cast<long long>(delivered_bps / 1000), static_cast<long long>(late / 1000));
  EXPECT_GT(late, std::min<int64_t>(700'000, delivered_bps * 7 / 10));
  EXPECT_GT(late, 300'000);
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

namespace {

struct CursorRun {
  bool ok = false;
  double idle_p50 = 0, idle_p95 = 0, p50 = 0, p95 = 0, kbps = 0;
  // What the load adds, over what the machine adds by itself. On Linux idle
  // is ~20 ms and loaded 30-38; before the sender drained the network's
  // queue, loaded was 68 at the same idle (a standing queue adds its length
  // on any machine).
  bool Passes() const { return ok && p50 < idle_p50 + 30 && p95 < idle_p95 + 70 && kbps > 1'200; }
};

// Video keeps the 1.5 Mbps path busy (as VideoToolbox does on the Mac);
// small cursor messages every 16 ms must not wait behind a standing queue
// inside the network: the sender's own queue lets them jump ahead.
CursorRun MeasureCursorUnderLoad() {
  net::LinkConditions c;
  c.bandwidth_bps = 1'500'000;
  c.delay_ms = 20;
  c.queue_ms = 200;
  std::mutex mu;
  std::vector<double> idle_ms, lat_ms;
  std::atomic<bool> loaded{false};
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
      (loaded ? lat_ms : idle_ms).push_back((now - sent) / 1000.0);
    }
  });
  const auto chunk = Pattern(6'000, 5);
  TransportStats st;
  // First a second of cursor messages alone, which shows what this machine
  // adds by itself (threads that wake late: on a loaded macOS runner the
  // round trip runs at several times the path's). Then six seconds with
  // video saturating the path.
  auto run = [&](uint64_t duration_us, bool video) {
    const uint64_t t0 = capture::MonotonicMicros();
    uint64_t next_cursor = t0;
    while (capture::MonotonicMicros() - t0 < duration_us) {
      const uint64_t now = capture::MonotonicMicros();
      host.GetStats(&st);
      if (video && st.queued_bytes < 30'000) host.Send(Channel::kVideo, chunk);
      if (now >= next_cursor) {
        uint8_t msg[8];
        std::memcpy(msg, &now, 8);
        host.Send(Channel::kCursor, msg);
        next_cursor += 16'000;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  run(1'000'000, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  loaded = true;
  run(6'000'000, true);
  std::lock_guard<std::mutex> lock(mu);
  if (idle_ms.size() <= 30) return {};
  std::sort(idle_ms.begin(), idle_ms.end());
  const double idle_p50 = idle_ms[idle_ms.size() / 2], idle_p95 = idle_ms[idle_ms.size() * 95 / 100];
  // Past the start-up phase.
  std::vector<double> v(lat_ms.begin() + std::min<size_t>(lat_ms.size(), 120), lat_ms.end());
  if (v.size() <= 100) return {};
  std::sort(v.begin(), v.end());
  const double p50 = v[v.size() / 2], p95 = v[v.size() * 95 / 100];
  const double kbps = static_cast<double>(video_bytes.load()) * 8 / 6.0 / 1000;
  CursorRun r{true, idle_p50, idle_p95, p50, p95, kbps};
  std::printf("cursor idle: p50 %.1f p95 %.1f ms; under load: p50 %.1f p95 %.1f ms, video %.0f kbps, rtt %u ms\n",
              idle_p50, idle_p95, p50, p95, kbps, static_cast<unsigned>(st.rtt_us / 1000));
  return r;
}

}  // namespace

TEST(ReliableTransport, CursorStaysFastUnderASaturatingSender) {
  // A loaded macOS runner sometimes stalls threads for tens of ms in the
  // middle of a run (p50 62.5 ms once, against a 50 ms limit). A standing
  // queue fails every run, a stall rarely two in a row: so up to three runs,
  // and the test passes on the first that meets the limits.
  CursorRun r;
  for (int attempt = 0; attempt < 3 && !r.Passes(); ++attempt) r = MeasureCursorUnderLoad();
  ASSERT_TRUE(r.ok) << "too few cursor messages arrived";
  EXPECT_LT(r.p50, r.idle_p50 + 30);
  EXPECT_LT(r.p95, r.idle_p95 + 70);
  // Without giving up the path's capacity.
  EXPECT_GT(r.kbps, 1'200);
}

namespace {

TransportStats SendThrough(const net::LinkConditions& c, size_t bytes, int fec = -1) {
  Sink at_viewer, at_host;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = c.delay_ms, .jitter_ms = c.jitter_ms});
  net::ReliableTransport host(std::move(a), {.fec = fec});
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(at_viewer.Fn());
  host.SetReceiver(at_host.Fn());
  const auto video = Pattern(bytes, 7);
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < bytes; i += 20'000) host.Send(Channel::kVideo, std::span(video).subspan(i, std::min<size_t>(20'000, bytes - i)));
  EXPECT_TRUE(at_viewer.WaitFor(3, video.size()));
  EXPECT_EQ(at_viewer.got[3], video);
  std::printf("  %zu bytes in %.1f s\n", bytes,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  TransportStats st;
  EXPECT_TRUE(host.GetStats(&st));
  return st;
}

}  // namespace

TEST(ReliableTransport, LateIsNotLostOnAJitteryPath) {
  // Jittery Wi-Fi: no packet is lost, but each one waits 0-60 ms extra, so
  // they arrive out of order and late. Every resend is then spurious, and
  // counted as such.
  net::LinkConditions c;
  c.bandwidth_bps = 8'000'000;
  c.delay_ms = 15;
  c.jitter_ms = 60;
  const TransportStats st = SendThrough(c, 2'000'000);
  std::printf("jittery: sent %llu lost %llu spurious %llu\n", (unsigned long long)st.packets_sent,
              (unsigned long long)st.packets_lost, (unsigned long long)st.packets_spurious);
  EXPECT_GT(st.packets_spurious, 0u);  // seen, and counted
  EXPECT_LE(st.packets_lost, st.packets_spurious + st.packets_sent / 100);  // none was really lost
}

TEST(ReliableTransport, RealLossIsNotTakenForLateness) {
  net::LinkConditions c;
  c.bandwidth_bps = 8'000'000;
  c.delay_ms = 20;
  c.loss = 0.02;
  // Without forward error correction: a packet it rebuilds after the loss
  // was declared would be acknowledged "late" too.
  const TransportStats st = SendThrough(c, 2'000'000, /*fec=*/0);
  std::printf("lossy: sent %llu lost %llu spurious %llu\n", (unsigned long long)st.packets_sent,
              (unsigned long long)st.packets_lost, (unsigned long long)st.packets_spurious);
  EXPECT_GT(st.packets_lost, st.packets_sent / 100);  // the 2% are found
  EXPECT_LE(st.packets_spurious, st.packets_lost / 10);
}

TEST(ReliableTransport, ReorderingIsNotLossOnALongPath) {
  // A long path (a VPN across an ocean) whose packets each wait 0-20 ms
  // extra, so tens of them arrive out of order. With the packet rule alone
  // nearly every one was resent (half the path went to resends); the
  // reorder window learnt from the first late ones stops that.
  net::LinkConditions c;
  c.bandwidth_bps = 10'000'000;
  c.delay_ms = 100;
  c.jitter_ms = 20;
  const TransportStats st = SendThrough(c, 3'000'000);
  std::printf("reordered: sent %llu lost %llu spurious %llu\n", (unsigned long long)st.packets_sent,
              (unsigned long long)st.packets_lost, (unsigned long long)st.packets_spurious);
  EXPECT_LT(st.packets_spurious, st.packets_sent / 20);  // was ~45%
}

TEST(ReliableTransport, RealLossIsFoundDespiteReordering) {
  // The same reordering with 2% real loss: the window must not hide it.
  net::LinkConditions c;
  c.bandwidth_bps = 10'000'000;
  c.delay_ms = 100;
  c.jitter_ms = 20;
  c.loss = 0.02;
  const TransportStats st = SendThrough(c, 3'000'000, /*fec=*/0);
  std::printf("reordered, lossy: sent %llu lost %llu spurious %llu\n", (unsigned long long)st.packets_sent,
              (unsigned long long)st.packets_lost, (unsigned long long)st.packets_spurious);
  EXPECT_GT(st.packets_lost - std::min(st.packets_lost, st.packets_spurious), st.packets_sent / 100);
  EXPECT_LT(st.packets_spurious, st.packets_sent / 20);
}

namespace {

struct FrameLatency {
  double p50 = 0, p95 = 0, max = 0;
  TransportStats st;
};

// Pictures of 6 kB every 33 ms over a long, lossy path: how long each takes
// to arrive whole (what a viewer waits for before it can decode it).
FrameLatency PicturesOverLossyPath(int fec) {
  net::LinkConditions c;
  c.bandwidth_bps = 8'000'000;
  c.delay_ms = 100;
  c.loss = 0.03;
  c.seed = 11;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = 100, .loss = 0.03, .seed = 12});
  net::ReliableTransport host(std::move(a), {.fec = fec});
  net::ReliableTransport viewer(std::move(b), {.client = true, .fec = fec});
  constexpr size_t kFrame = 6'000;
  constexpr int kFrames = 150;
  std::mutex mu;
  std::condition_variable cv;
  size_t got = 0;
  std::vector<std::chrono::steady_clock::time_point> sent(kFrames), arrived(kFrames);
  viewer.SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) {
    if (ch != Channel::kVideo) return;
    std::lock_guard<std::mutex> l(mu);
    const size_t before = got / kFrame;
    got += bytes.size();
    for (size_t k = before; k < got / kFrame && k < kFrames; ++k) arrived[k] = std::chrono::steady_clock::now();
    cv.notify_all();
  });
  host.SetReceiver([](Channel, std::span<const uint8_t>) {});
  const auto frame = Pattern(kFrame, 9);
  for (int k = 0; k < kFrames; ++k) {
    {
      std::lock_guard<std::mutex> l(mu);
      sent[k] = std::chrono::steady_clock::now();
    }
    host.Send(Channel::kVideo, frame);
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  {
    std::unique_lock<std::mutex> l(mu);
    EXPECT_TRUE(cv.wait_for(l, std::chrono::seconds(20), [&] { return got >= kFrame * kFrames; }));
  }
  // The first second fills the window and settles the loss estimate.
  std::vector<double> ms;
  for (int k = 30; k < kFrames; ++k) ms.push_back(std::chrono::duration<double, std::milli>(arrived[k] - sent[k]).count());
  std::sort(ms.begin(), ms.end());
  FrameLatency r;
  r.p50 = ms[ms.size() / 2];
  r.p95 = ms[ms.size() * 95 / 100];
  r.max = ms.back();
  host.GetStats(&r.st);
  return r;
}

}  // namespace

TEST(ReliableTransport, ForwardErrorCorrectionCutsTheWaitForLostPackets) {
  // 200 ms round trip, 3% loss: without FEC every picture behind a lost
  // packet waits a round trip more for its resend.
  const FrameLatency off = PicturesOverLossyPath(0);
  const FrameLatency on = PicturesOverLossyPath(-1);  // automatic: long, lossy path
  std::printf("pictures, FEC off: p50 %.0f p95 %.0f max %.0f ms\n", off.p50, off.p95, off.max);
  std::printf("pictures, FEC on:  p50 %.0f p95 %.0f max %.0f ms (%llu parities, %llu rebuilt)\n", on.p50, on.p95,
              on.max, static_cast<unsigned long long>(on.st.fec_parity_sent),
              static_cast<unsigned long long>(on.st.fec_recovered));
  EXPECT_EQ(off.st.fec_parity_sent, 0u);
  EXPECT_GT(on.st.fec_parity_sent, 0u);
  EXPECT_GT(on.st.fec_recovered, 0u);
  EXPECT_LT(on.p95, off.p95);
}

TEST(ReliableTransport, BulkGoesAfterTheVideoAndIsNotAQueueForTheHost) {
  net::LinkConditions c;
  c.bandwidth_bps = 10'000'000;
  c.delay_ms = 30;
  Sink at_viewer, at_host;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = c.delay_ms});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(at_viewer.Fn());
  host.SetReceiver(at_host.Fn());
  const auto bulk = Pattern(3'000'000, 11);
  const auto video = Pattern(300'000, 12);
  // A big transfer first, then a picture: the picture overtakes it.
  for (size_t i = 0; i < bulk.size(); i += 100'000) host.Send(Channel::kBulk, std::span(bulk).subspan(i, 100'000));
  TransportStats st;
  ASSERT_TRUE(host.GetStats(&st));
  EXPECT_TRUE(st.bulk_known);
  EXPECT_GT(st.bulk_queued_bytes, 2'000'000u);
  EXPECT_LT(st.queued_bytes, 100'000u);  // the host must not hold frames back for it
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < video.size(); i += 20'000) host.Send(Channel::kVideo, std::span(video).subspan(i, 20'000));
  ASSERT_TRUE(at_viewer.WaitFor(3, video.size()));
  const double video_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  {
    std::lock_guard<std::mutex> lock(at_viewer.mu);
    EXPECT_LT(at_viewer.got[6].size(), bulk.size() / 2) << "the picture waited for the transfer";
  }
  // 300 kB at 10 Mbps is 0.24 s; behind 3 MB of bulk it would be 2.4 s.
  EXPECT_LT(video_s, 0.9);
  ASSERT_TRUE(at_viewer.WaitFor(6, bulk.size()));
  EXPECT_EQ(at_viewer.got[6], bulk);
  EXPECT_EQ(at_viewer.got[3], video);
}

namespace {

// Input sent while a bulk transfer runs over a path of `delay_ms` one way:
// how late it is (median and worst, ms) and the transfer's speed (Mbps).
struct InputDuringBulk {
  double median_ms = 0, worst_ms = 0, bulk_mbps = 0;
};

InputDuringBulk MeasureInputDuringBulk(int32_t delay_ms, size_t bulk_bytes, int warmup_ms, int32_t jitter_ms = 0);

}  // namespace

TEST(ReliableTransport, BulkLeavesNoQueueForInput) {
  // A transfer takes what the path has to spare without building a queue at
  // the bottleneck: input (and the picture's acknowledgements) sent the same
  // way meanwhile must not wait behind it there. Reported over a VPN: the
  // whole remote desktop lagged while a file was copied.
  const InputDuringBulk r = MeasureInputDuringBulk(20, 8'000'000, 1500);
  std::printf("  input during bulk: median %.0f ms, worst %.0f ms one way (path 20 ms); bulk %.1f Mbps\n", r.median_ms,
              r.worst_ms, r.bulk_mbps);
  EXPECT_LT(r.median_ms, 20 + 15.0) << "input waited in a queue the transfer built";
  EXPECT_GT(r.bulk_mbps, 6.5) << "the transfer must still use the path";
}

TEST(ReliableTransport, BulkLeavesNoQueueForInputOnAFarPath) {
  // The path the lag was reported on: about 190 ms round trips. A queue of a
  // quarter of the round trip is 50 ms here.
  const InputDuringBulk r = MeasureInputDuringBulk(95, 16'000'000, 6000);
  std::printf("  input during bulk: median %.0f ms, worst %.0f ms one way (path 95 ms); bulk %.1f Mbps\n", r.median_ms,
              r.worst_ms, r.bulk_mbps);
  EXPECT_LT(r.median_ms, 95 + 15.0) << "input waited in a queue the transfer built";
  EXPECT_GT(r.bulk_mbps, 5.0) << "the transfer must still use the path";
}

namespace {

InputDuringBulk MeasureInputDuringBulk(int32_t delay_ms, size_t bulk_bytes, int warmup_ms, int32_t jitter_ms) {
  net::LinkConditions c;
  c.bandwidth_bps = 10'000'000;
  c.delay_ms = delay_ms;
  c.jitter_ms = jitter_ms;
  c.queue_ms = 300;
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::chrono::steady_clock::time_point> arrived;
  size_t bulk_got = 0;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = c.delay_ms});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver([&](Channel ch, std::span<const uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(mu);
    if (ch == Channel::kInput) arrived.push_back(std::chrono::steady_clock::now());
    if (ch == Channel::kBulk) bulk_got += bytes.size();
    cv.notify_all();
  });
  Sink at_host;
  host.SetReceiver(at_host.Fn());
  const auto bulk = Pattern(bulk_bytes, 21);
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < bulk.size(); i += 100'000) host.Send(Channel::kBulk, std::span(bulk).subspan(i, 100'000));
  std::this_thread::sleep_for(std::chrono::milliseconds(warmup_ms));  // the transfer is in full swing
  std::vector<double> delays_ms;
  const uint8_t key[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  for (int i = 0; i < 20; ++i) {
    const auto sent = std::chrono::steady_clock::now();
    host.Send(Channel::kInput, std::span<const uint8_t>(key, sizeof key));
    std::unique_lock<std::mutex> lock(mu);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return arrived.size() > static_cast<size_t>(i); }));
    if (arrived.size() <= static_cast<size_t>(i)) return {};
    delays_ms.push_back(std::chrono::duration<double, std::milli>(arrived[static_cast<size_t>(i)] - sent).count());
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  {
    std::unique_lock<std::mutex> lock(mu);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(60), [&] { return bulk_got >= bulk.size(); }));
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::sort(delays_ms.begin(), delays_ms.end());
  InputDuringBulk r;
  r.median_ms = delays_ms[delays_ms.size() / 2];
  r.worst_ms = delays_ms.back();
  r.bulk_mbps = bulk.size() * 8 / s / 1e6;
  return r;
}

}  // namespace

// Measurement only (--gtest_also_run_disabled_tests): the far path with a
// few milliseconds of jitter, as a real VPN has.
TEST(ReliableTransport, DISABLED_InputDuringBulkOnAJitteryFarPath) {
  for (int32_t jitter : {0, 3, 8}) {
    const InputDuringBulk r = MeasureInputDuringBulk(95, 16'000'000, 6000, jitter);
    std::printf("  jitter %d ms: input median %.0f ms, worst %.0f ms one way (path 95 ms); bulk %.1f Mbps\n", jitter,
                r.median_ms, r.worst_ms, r.bulk_mbps);
  }
}

TEST(ReliableTransport, BulkUsesAFarPath) {
  // 500 ms round trips, 10 Mbps: the transport's own congestion control,
  // not a delay guess, sets the pace of a bulk transfer. (The simulated
  // link's jitter delays each packet on its own, which reorders them by up
  // to the jitter: with 20 ms of it the same transfer reaches only about
  // 4.6 Mbps, the reordering taken for loss. Real paths keep their order.)
  net::LinkConditions c;
  c.bandwidth_bps = 10'000'000;
  c.delay_ms = 250;
  Sink at_viewer, at_host;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = c.delay_ms, .jitter_ms = c.jitter_ms});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(at_viewer.Fn());
  host.SetReceiver(at_host.Fn());
  const auto bulk = Pattern(10'000'000, 13);
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < bulk.size(); i += 100'000) host.Send(Channel::kBulk, std::span(bulk).subspan(i, 100'000));
  ASSERT_TRUE(at_viewer.WaitFor(6, bulk.size(), 60));
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double mbps = bulk.size() * 8 / s / 1e6;
  std::printf("  10 MB of bulk over 500 ms round trips: %.1f s, %.1f Mbps\n", s, mbps);
  EXPECT_GT(mbps, 5.0);
  EXPECT_EQ(at_viewer.got[6], bulk);
}

namespace {

struct BulkRun {
  double mbps = 0;
  TransportStats st;
};

// A large transfer over the simulated link: goodput and what was resent.
BulkRun BulkThrough(const net::LinkConditions& c, size_t bytes) {
  Sink at_viewer, at_host;
  net::LinkConditions back{.delay_ms = c.delay_ms, .jitter_ms = c.jitter_ms, .seed = c.seed + 1};
  auto [a, b] = net::MakeSimulatedLink(c, back);
  net::ReliableTransport host(std::move(a), {});
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(at_viewer.Fn());
  host.SetReceiver(at_host.Fn());
  const auto data = Pattern(bytes, 5);
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < bytes; i += 64'000) {
    host.Send(Channel::kVideo, std::span(data).subspan(i, std::min<size_t>(64'000, bytes - i)));
  }
  BulkRun r;
  EXPECT_TRUE(at_viewer.WaitFor(3, bytes, 120));
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  r.mbps = static_cast<double>(bytes) * 8 / s / 1e6;
  EXPECT_TRUE(host.GetStats(&r.st));
  return r;
}

}  // namespace

// Measurement only (run with --gtest_also_run_disabled_tests): goodput of a
// bulk transfer over a long path with and without reordering.
TEST(ReliableTransport, DISABLED_BulkGoodputUnderReordering) {
  for (int delay : {100, 250}) {
    for (int jitter : {0, 5, 20, 50}) {
      for (double loss : {0.0, 0.01}) {
        net::LinkConditions c{.bandwidth_bps = 10'000'000, .delay_ms = delay, .jitter_ms = jitter, .loss = loss};
        const BulkRun r = BulkThrough(c, 6'000'000);
        std::printf("rtt %3d ms jitter %2d ms loss %.0f%%: %5.2f Mbps, sent %llu lost %llu spurious %llu\n",
                    2 * delay, jitter, loss * 100, r.mbps, (unsigned long long)r.st.packets_sent,
                    (unsigned long long)r.st.packets_lost, (unsigned long long)r.st.packets_spurious);
      }
    }
  }
}

TEST(UdpSocket, MarksBulkAsBackgroundAndTheRestAsInteractive) {
  // A plain socket on loopback that is told each datagram's TOS byte.
  const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(rx, 0);
  int one = 1;
  ASSERT_EQ(::setsockopt(rx, IPPROTO_IP, IP_RECVTOS, &one, sizeof(one)), 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(rx, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(::getsockname(rx, reinterpret_cast<sockaddr*>(&addr), &len), 0);
  std::string error;
  auto tx = net::ConnectUdpSocket("127.0.0.1", ntohs(addr.sin_port), &error);
  ASSERT_TRUE(tx) << error;
  auto tos_of_next = [&]() -> int {
    uint8_t buf[2048];
    alignas(cmsghdr) uint8_t control[256];
    iovec iov{buf, sizeof buf};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    if (::recvmsg(rx, &msg, 0) <= 0) return -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == IPPROTO_IP && (c->cmsg_type == IP_RECVTOS || c->cmsg_type == IP_TOS)) {
        return *reinterpret_cast<const uint8_t*>(CMSG_DATA(c));
      }
    }
    return -2;
  };
  const uint8_t d[4] = {1, 2, 3, 4};
  tx->Send(std::span<const uint8_t>(d, sizeof d), net::TrafficClass::kBackground);
  EXPECT_EQ(tos_of_next() & 0xFC, 0x20);  // DSCP CS1
  tx->Send(std::span<const uint8_t>(d, sizeof d), net::TrafficClass::kInteractive);
  EXPECT_EQ(tos_of_next() & 0xFC, 0x80);  // DSCP CS4
  tx->Send(std::span<const uint8_t>(d, sizeof d));  // unmarked sends are interactive
  EXPECT_EQ(tos_of_next() & 0xFC, 0x80);
  tx->Close();
  ::close(rx);
}


// ---- datagrams (1.13) -------------------------------------------------------

namespace {

struct DatagramSink {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::vector<uint8_t>> got;
  std::vector<uint64_t> times;  // when each arrived

  transport::ITransport::DatagramFn Fn() {
    return [this](std::span<const uint8_t> b) {
      std::lock_guard<std::mutex> lock(mu);
      got.emplace_back(b.begin(), b.end());
      times.push_back(capture::MonotonicMicros());
      cv.notify_all();
    };
  }
  size_t count() {
    std::lock_guard<std::mutex> lock(mu);
    return got.size();
  }
};

// A datagram that says which one it is and when it was sent.
std::vector<uint8_t> NumberedDatagram(uint32_t n, size_t size = 200) {
  std::vector<uint8_t> d(size, static_cast<uint8_t>(n));
  const uint64_t t = capture::MonotonicMicros();
  std::memcpy(d.data(), &n, 4);
  std::memcpy(d.data() + 4, &t, 8);
  return d;
}

}  // namespace

TEST(ReliableTransport, DatagramsArriveAndAreNeverResent) {
  net::LinkConditions c;
  c.bandwidth_bps = 20'000'000;
  c.delay_ms = 10;
  c.loss = 0.05;
  c.seed = 7;
  DatagramSink sink;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = 10});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetDatagramReceiver(sink.Fn());
  EXPECT_TRUE(host.CarriesDatagrams());
  // The listening host learns its peer from the viewer's hello first.
  for (int i = 0; i < 200 && !host.PeerKnown(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(host.PeerKnown());
  constexpr uint32_t kCount = 2000;
  for (uint32_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(host.SendDatagram(NumberedDatagram(i)));
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::lock_guard<std::mutex> lock(sink.mu);
  std::vector<int> seen(kCount, 0);
  for (const auto& d : sink.got) {
    uint32_t n = 0;
    std::memcpy(&n, d.data(), 4);
    ASSERT_LT(n, kCount);
    ++seen[n];
    EXPECT_EQ(d.size(), 200u);
  }
  const auto received = static_cast<double>(std::count(seen.begin(), seen.end(), 1));
  EXPECT_EQ(std::count_if(seen.begin(), seen.end(), [](int k) { return k > 1; }), 0) << "a datagram came twice";
  EXPECT_GT(received / kCount, 0.92);
  EXPECT_LT(received / kCount, 0.98);
  TransportStats st;
  ASSERT_TRUE(host.GetStats(&st));
  EXPECT_EQ(st.datagrams_sent, kCount);
  EXPECT_GT(st.packets_lost, 0u);  // lost and counted, not resent
  EXPECT_LT(st.packets_sent, kCount + 50);
  EXPECT_FALSE(host.SendDatagram(std::vector<uint8_t>(kMaxDatagramPayload + 1)));
}

namespace {

struct DatagramRun {
  bool ok = false;
  double idle_p50 = 0, idle_p95 = 0, p50 = 0, p95 = 0, p99 = 0;
  int64_t bandwidth_bps = 0;
  // As for the cursor channel (CursorRun): what the load adds over what the
  // machine adds by itself.
  bool Passes() const { return ok && p50 < idle_p50 + 20 && p95 < idle_p95 + 70; }
};

// Video keeps a 4 Mbps path busy; datagrams every 10 ms (sound) go out ahead
// of it, so they wait at most behind what the picture keeps queued inside
// the network, which the congestion control keeps short.
DatagramRun MeasureDatagramsUnderLoad() {
  net::LinkConditions c;
  c.bandwidth_bps = 4'000'000;
  c.delay_ms = 20;
  c.queue_ms = 300;
  DatagramSink sink;
  Sink video_sink;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = 20});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetReceiver(video_sink.Fn());
  viewer.SetDatagramReceiver(sink.Fn());
  const auto chunk = Pattern(10'000, 9);
  TransportStats st;
  uint32_t n = 0;
  // A second of datagrams alone (what the machine adds by itself), then
  // four with the picture saturating the path.
  auto run = [&](uint64_t duration_us, bool video) {
    const uint64_t t0 = capture::MonotonicMicros();
    uint64_t next_datagram = t0;
    while (capture::MonotonicMicros() - t0 < duration_us) {
      host.GetStats(&st);
      if (video && st.queued_bytes < 30'000) host.Send(Channel::kVideo, chunk);
      const uint64_t now = capture::MonotonicMicros();
      if (now >= next_datagram) {
        host.SendDatagram(NumberedDatagram(n++, 300));  // 100 a second: about 240 kbps
        next_datagram += 10'000;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  run(1'000'000, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const uint32_t loaded_from = n;
  run(4'000'000, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::vector<double> idle, loaded;
  {
    std::lock_guard<std::mutex> lock(sink.mu);
    for (size_t i = 0; i < sink.got.size(); ++i) {
      uint32_t k = 0;
      uint64_t sent = 0;
      std::memcpy(&k, sink.got[i].data(), 4);
      std::memcpy(&sent, sink.got[i].data() + 4, 8);
      // The loaded part past the estimate's start-up (its first second).
      if (k < loaded_from) {
        idle.push_back((sink.times[i] - sent) / 1000.0);
      } else if (k >= loaded_from + 100) {
        loaded.push_back((sink.times[i] - sent) / 1000.0);
      }
    }
  }
  if (idle.size() < 80 || loaded.size() < static_cast<size_t>(n - loaded_from - 100) * 9 / 10) return {};
  std::sort(idle.begin(), idle.end());
  std::sort(loaded.begin(), loaded.end());
  host.GetStats(&st);
  DatagramRun r{true,
                idle[idle.size() / 2],
                idle[idle.size() * 95 / 100],
                loaded[loaded.size() / 2],
                loaded[loaded.size() * 95 / 100],
                loaded[loaded.size() * 99 / 100],
                st.bandwidth_bps};
  std::printf("datagrams idle: p50 %.1f p95 %.1f ms; under load: p50 %.1f p95 %.1f p99 %.1f ms, estimate %.2f Mbps, rtt %u ms\n",
              r.idle_p50, r.idle_p95, r.p50, r.p95, r.p99, r.bandwidth_bps / 1e6, static_cast<unsigned>(st.rtt_us / 1000));
  return r;
}

}  // namespace

TEST(ReliableTransport, DatagramsGoAheadOfASaturatingPicture) {
  // The sender's own queue is skipped altogether; what is left is the queue
  // the picture keeps inside the network (10-15 ms at the median here).
  // Retried as the cursor test is: a loaded machine stalls threads now and
  // then, a standing queue fails every run.
  DatagramRun r;
  for (int attempt = 0; attempt < 3 && !r.Passes(); ++attempt) r = MeasureDatagramsUnderLoad();
  ASSERT_TRUE(r.ok) << "too few datagrams arrived";
  EXPECT_LT(r.p50, r.idle_p50 + 20);
  EXPECT_LT(r.p95, r.idle_p95 + 70);
  EXPECT_GT(r.bandwidth_bps, 3'000'000);  // the picture still fills the path
}

TEST(ReliableTransport, DatagramsAloneMakeAnEstimate) {
  net::LinkConditions c;
  c.bandwidth_bps = 10'000'000;
  c.delay_ms = 10;
  DatagramSink sink;
  auto [a, b] = net::MakeSimulatedLink(c, net::LinkConditions{.delay_ms = 10});
  net::ReliableTransport host(std::move(a));
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetDatagramReceiver(sink.Fn());
  for (int i = 0; i < 200 && !host.PeerKnown(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  TransportStats st;
  bool in_flight_seen = false;
  for (uint32_t i = 0; i < 100; ++i) {
    host.SendDatagram(NumberedDatagram(i, 400));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    host.GetStats(&st);
    in_flight_seen |= st.bytes_in_flight > 0;
  }
  host.GetStats(&st);
  EXPECT_TRUE(in_flight_seen);
  EXPECT_GT(st.bandwidth_bps, 0);
  EXPECT_GT(st.rtt_us, 15'000u);
  EXPECT_EQ(st.datagrams_sent, 100u);
  EXPECT_GT(st.datagram_bytes_sent, 100u * 400);
  EXPECT_GT(sink.count(), 95u);
}

TEST(ReliableTransport, DatagramsThatWaitTooLongAreDropped) {
  // No viewer yet: the listening host cannot send, so what waits over
  // 100 ms goes, and none of it arrives late once a viewer comes.
  auto [a, b] = net::MakeSimulatedLink({}, {});
  net::ReliableTransport host(std::move(a));
  for (uint32_t i = 0; i < 10; ++i) ASSERT_TRUE(host.SendDatagram(NumberedDatagram(i)));
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  DatagramSink sink;
  net::ReliableTransport viewer(std::move(b), {.client = true});
  viewer.SetDatagramReceiver(sink.Fn());
  for (int i = 0; i < 200 && !host.PeerKnown(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(host.SendDatagram(NumberedDatagram(99)));
  for (int i = 0; i < 100 && sink.count() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  TransportStats st;
  host.GetStats(&st);
  EXPECT_EQ(st.datagrams_dropped, 10u);
  std::lock_guard<std::mutex> lock(sink.mu);
  ASSERT_EQ(sink.got.size(), 1u);
  EXPECT_EQ(sink.got[0][0], 99);
}

TEST(ReliableTransport, NoDatagramsLeavesTheStreamsAsTheyWere) {
  // Without datagrams nothing of theirs is sent or counted.
  const TransportStats st = SendThrough(net::LinkConditions{.bandwidth_bps = 10'000'000, .delay_ms = 10}, 200'000);
  EXPECT_EQ(st.datagrams_sent, 0u);
  EXPECT_EQ(st.datagram_bytes_sent, 0u);
  EXPECT_EQ(st.datagrams_dropped, 0u);
}
