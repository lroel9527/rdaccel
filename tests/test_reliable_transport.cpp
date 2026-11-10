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
