#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

#include "net/datagram.h"

namespace rdacore::net {
namespace {

std::string Errno(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

class UdpSocket final : public DatagramSocket {
 public:
  // `first` is a datagram already read from `fd`. An unconnected `fd` gets
  // connected to whoever sends the first datagram; until then sends are
  // dropped.
  UdpSocket(int fd, std::vector<uint8_t> first, bool connected)
      : fd_(fd), first_(std::move(first)), connected_(connected) {
    int buf = 4 << 20;  // room for bursts at high bitrates
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  }
  ~UdpSocket() override {
    Close();
    ::close(fd_);
  }

  bool Send(std::span<const uint8_t> d) override {
    if (closed_) return false;
    if (!connected_) return true;  // no peer yet: lost, like any datagram
    // Errors (e.g. ICMP port unreachable, full buffer) count as loss.
    (void)::send(fd_, d.data(), d.size(), 0);
    return true;
  }

  void SetReceiver(ReceiveFn fn) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable()) return;
    receiver_ = std::move(fn);
    thread_ = std::thread([this] { Run(); });
  }

  void Close() override {
    closed_ = true;
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
    if (!first_.empty()) receiver_(first_);
    std::vector<uint8_t> buf(65536);
    while (!closed_) {
      pollfd p{fd_, POLLIN, 0};
      const int rc = ::poll(&p, 1, 50);  // wake up regularly to notice Close()
      if (rc <= 0) continue;
      if (!connected_) {
        sockaddr_storage peer{};
        socklen_t len = sizeof(peer);
        const ssize_t n =
            ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&peer), &len);
        if (n <= 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&peer), len) < 0) continue;
        connected_ = true;
        receiver_({buf.data(), static_cast<size_t>(n)});
        continue;
      }
      const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
      if (n > 0) receiver_({buf.data(), static_cast<size_t>(n)});
    }
  }

  int fd_;
  std::vector<uint8_t> first_;
  std::atomic<bool> connected_;
  std::atomic<bool> closed_{false};
  std::mutex mu_;
  ReceiveFn receiver_;
  std::thread thread_;
};

}  // namespace

std::unique_ptr<DatagramSocket> ListenUdpSocket(uint16_t port, std::string* error) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    if (error) *error = Errno("socket");
    return nullptr;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    if (error) *error = Errno("bind");
    ::close(fd);
    return nullptr;
  }
  std::vector<uint8_t> first(65536);
  sockaddr_storage peer{};
  socklen_t len = sizeof(peer);
  ssize_t n;
  do {
    n = ::recvfrom(fd, first.data(), first.size(), 0, reinterpret_cast<sockaddr*>(&peer), &len);
  } while (n < 0 && errno == EINTR);
  if (n < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&peer), len) < 0) {
    if (error) *error = Errno("recvfrom/connect");
    ::close(fd);
    return nullptr;
  }
  first.resize(static_cast<size_t>(n));
  return std::make_unique<UdpSocket>(fd, std::move(first), true);
}

std::unique_ptr<DatagramSocket> BindUdpSocket(uint16_t port, std::string* error) {
  const int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
  int dual = 0;
  bool v6 = fd >= 0 && ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &dual, sizeof(dual)) == 0;
  int s = fd;
  if (!v6) {
    if (fd >= 0) ::close(fd);
    s = ::socket(AF_INET, SOCK_DGRAM, 0);
  }
  if (s < 0) {
    if (error) *error = Errno("socket");
    return nullptr;
  }
  int one = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int rc;
  if (v6) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    rc = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    rc = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  }
  if (rc < 0) {
    if (error) *error = Errno("bind");
    ::close(s);
    return nullptr;
  }
  return std::make_unique<UdpSocket>(s, std::vector<uint8_t>{}, false);
}

std::unique_ptr<DatagramSocket> UdpSocketFromFd(int fd, std::string* error) {
  int type = 0;
  socklen_t tlen = sizeof(type);
  if (fd < 0 || ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) < 0 || type != SOCK_DGRAM) {
    if (error) *error = "not a UDP socket";
    return nullptr;
  }
  sockaddr_storage peer{};
  socklen_t len = sizeof(peer);
  const bool connected = ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len) == 0;
  return std::make_unique<UdpSocket>(fd, std::vector<uint8_t>{}, connected);
}

std::unique_ptr<DatagramSocket> ConnectUdpSocket(const std::string& host, uint16_t port,
                                                 std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(port);
  if (const int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res); rc != 0) {
    if (error) *error = std::string("getaddrinfo: ") + ::gai_strerror(rc);
    return nullptr;
  }
  int fd = -1;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) {
    if (error) *error = Errno("connect");
    return nullptr;
  }
  return std::make_unique<UdpSocket>(fd, std::vector<uint8_t>{}, true);
}

}  // namespace rdacore::net
