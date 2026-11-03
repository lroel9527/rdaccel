#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <random>
#include <thread>

#include "capture/capturer.h"
#include "net/datagram.h"

namespace rdacore::net {
namespace {

// One direction of the link: a bottleneck that serializes datagrams at the
// current bandwidth, a drop-tail queue in front of it, then fixed delay plus
// jitter. A thread delivers each datagram at its arrival time.
class Direction {
 public:
  Direction(const LinkConditions& c)  // NOLINT: brace-initialised in Shared
      : c_(c), rng_(c.seed), start_(capture::MonotonicMicros()), thread_([this] { Run(); }) {}

  ~Direction() { Stop(); }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_) return;
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  void SetReceiver(DatagramSocket::ReceiveFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    receiver_ = std::move(fn);
  }

  bool Send(std::span<const uint8_t> d) {
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_) return false;
    ++stats_.sent;
    const uint64_t now = capture::MonotonicMicros();
    if (std::uniform_real_distribution<double>(0, 1)(rng_) < c_.loss) {
      ++stats_.lost;
      return true;
    }
    uint64_t depart = now;
    const int64_t bw = Bandwidth(now);
    if (bw > 0) {
      const uint64_t free_at = std::max(now, link_free_);
      // Queue full: tail drop.
      if (free_at - now > static_cast<uint64_t>(c_.queue_ms) * 1000) {
        ++stats_.dropped;
        return true;
      }
      link_free_ = free_at + static_cast<uint64_t>(d.size()) * 8 * 1'000'000 / static_cast<uint64_t>(bw);
      depart = link_free_;
    }
    uint64_t arrive = depart + static_cast<uint64_t>(c_.delay_ms) * 1000;
    if (c_.jitter_ms > 0) {
      arrive += std::uniform_int_distribution<uint64_t>(0, static_cast<uint64_t>(c_.jitter_ms) * 1000)(rng_);
    }
    queue_.push({arrive, seq_++, {d.begin(), d.end()}});
    cv_.notify_all();
    return true;
  }

  LinkStats stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
  }

 private:
  struct Item {
    uint64_t at;
    uint64_t seq;
    std::vector<uint8_t> bytes;
    bool operator>(const Item& o) const { return at != o.at ? at > o.at : seq > o.seq; }
  };

  int64_t Bandwidth(uint64_t now) const {
    int64_t bw = c_.bandwidth_bps;
    const auto ms = static_cast<int64_t>((now - start_) / 1000);
    for (const auto& [t, b] : c_.bandwidth_schedule) {
      if (ms >= t) bw = b;
    }
    return bw;
  }

  void Run() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
      if (stop_) return;
      if (queue_.empty()) {
        cv_.wait(lock);
        continue;
      }
      const uint64_t now = capture::MonotonicMicros();
      if (queue_.top().at > now) {
        cv_.wait_for(lock, std::chrono::microseconds(queue_.top().at - now));
        continue;
      }
      Item item = std::move(const_cast<Item&>(queue_.top()));
      queue_.pop();
      auto fn = receiver_;
      lock.unlock();
      if (fn) fn(item.bytes);
      lock.lock();
    }
  }

  LinkConditions c_;
  std::mt19937 rng_;
  uint64_t start_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  uint64_t link_free_ = 0;
  uint64_t seq_ = 0;
  std::priority_queue<Item, std::vector<Item>, std::greater<>> queue_;
  DatagramSocket::ReceiveFn receiver_;
  LinkStats stats_;
  std::thread thread_;  // last: starts after everything above is set up
};

struct Shared {
  Shared(const LinkConditions& ab, const LinkConditions& ba) : dir{{ab}, {ba}} {}
  Direction dir[2];
};

class SimSocket final : public DatagramSocket {
 public:
  SimSocket(std::shared_ptr<Shared> s, int self) : s_(std::move(s)), self_(self) {}
  ~SimSocket() override { Close(); }
  bool Send(std::span<const uint8_t> d) override {
    return !closed_ && s_->dir[self_].Send(d);
  }
  void SetReceiver(ReceiveFn fn) override { s_->dir[1 - self_].SetReceiver(std::move(fn)); }
  void Close() override {
    if (closed_) return;
    closed_ = true;
    // Stop delivering to this end (waits for a running callback).
    s_->dir[1 - self_].Stop();
  }

 private:
  std::shared_ptr<Shared> s_;
  int self_;
  bool closed_ = false;
};

}  // namespace

std::pair<std::unique_ptr<DatagramSocket>, std::unique_ptr<DatagramSocket>> MakeSimulatedLink(
    const LinkConditions& a_to_b, const LinkConditions& b_to_a) {
  auto s = std::make_shared<Shared>(a_to_b, b_to_a);
  return {std::make_unique<SimSocket>(s, 0), std::make_unique<SimSocket>(s, 1)};
}

}  // namespace rdacore::net
