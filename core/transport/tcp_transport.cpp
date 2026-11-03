#include "transport/tcp_transport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace rdacore::transport {
namespace {

constexpr uint32_t kMaxChunk = 64u << 20;
// Queued before the peer connects; the host holds frames meanwhile, so only
// handshake bytes and the newest frame are expected here.
constexpr size_t kMaxPending = 64u << 20;

std::string Errno(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

bool WriteAll(int fd, const uint8_t* p, size_t n) {
  while (n > 0) {
#ifdef MSG_NOSIGNAL
    const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
#else
    const ssize_t w = ::send(fd, p, n, 0);
#endif
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

bool ReadAll(int fd, uint8_t* p, size_t n) {
  while (n > 0) {
    const ssize_t r = ::recv(fd, p, n, 0);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

void Tune(int fd) {
  int one = 1;
  // Low latency matters more than packet count for interactive traffic.
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

}  // namespace

std::unique_ptr<TcpTransport> TcpTransport::Listen(uint16_t port, std::string* error) {
  const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) {
    if (error) *error = Errno("socket");
    return nullptr;
  }
  int one = 1;
  ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(ls, 1) < 0) {
    if (error) *error = Errno("bind/listen");
    ::close(ls);
    return nullptr;
  }
  int fd;
  do {
    fd = ::accept(ls, nullptr, nullptr);
  } while (fd < 0 && errno == EINTR);
  ::close(ls);
  if (fd < 0) {
    if (error) *error = Errno("accept");
    return nullptr;
  }
  Tune(fd);
  return std::unique_ptr<TcpTransport>(new TcpTransport(fd));
}

std::unique_ptr<TcpTransport> TcpTransport::Connect(const std::string& host, uint16_t port,
                                                    std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
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
  Tune(fd);
  return std::unique_ptr<TcpTransport>(new TcpTransport(fd));
}

std::unique_ptr<TcpTransport> TcpTransport::Bind(uint16_t port, std::string* error) {
  int one = 1;
  int ls = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (ls >= 0) {
    int zero = 0;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in6 a6{};
    a6.sin6_family = AF_INET6;
    a6.sin6_addr = in6addr_any;
    a6.sin6_port = htons(port);
    if (::setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)) != 0 ||
        ::bind(ls, reinterpret_cast<sockaddr*>(&a6), sizeof(a6)) != 0) {
      ::close(ls);
      ls = -1;
    }
  }
  if (ls < 0) {  // no IPv6: IPv4 only
    ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
      if (error) *error = Errno("socket");
      return nullptr;
    }
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a4{};
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    a4.sin_port = htons(port);
    if (::bind(ls, reinterpret_cast<sockaddr*>(&a4), sizeof(a4)) != 0) {
      if (error) *error = Errno("bind");
      ::close(ls);
      return nullptr;
    }
  }
  if (::listen(ls, 1) != 0) {
    if (error) *error = Errno("listen");
    ::close(ls);
    return nullptr;
  }
  return std::unique_ptr<TcpTransport>(new TcpTransport(Listening{}, ls));
}

TcpTransport::TcpTransport(int fd) : fd_(fd), reader_([this] { ReadLoop(); }) {}

TcpTransport::TcpTransport(Listening, int listen_fd)
    : fd_(-1), listen_fd_(listen_fd), reader_([this] {
        if (Accept()) ReadLoop();
        closed_ = true;
      }) {}

bool TcpTransport::Accept() {
  int fd = -1;
  while (!closed_) {
    // Polled with a timeout so Close() is noticed (closing a listening
    // socket does not wake accept() on every platform).
    pollfd p{listen_fd_, POLLIN, 0};
    const int r = ::poll(&p, 1, 100);
    if (r < 0 && errno != EINTR) return false;
    if (r <= 0) continue;
    fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd >= 0) break;
    if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN && errno != EWOULDBLOCK) return false;
  }
  ::close(listen_fd_);
  listen_fd_ = -1;
  if (fd < 0) return false;
  Tune(fd);
  // Flush what was sent meanwhile before any newer send can go out.
  std::lock_guard<std::mutex> send(send_mu_);
  {
    std::lock_guard<std::mutex> lock(fd_mu_);
    if (closed_) {
      ::close(fd);
      return false;
    }
    fd_ = fd;
  }
  const bool ok = WriteAll(fd, pending_.data(), pending_.size());
  pending_.clear();
  pending_.shrink_to_fit();
  return ok;
}

TcpTransport::~TcpTransport() {
  Close();
  if (reader_.joinable()) reader_.join();
  if (fd_ >= 0) ::close(fd_);
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

bool TcpTransport::Send(Channel ch, std::span<const uint8_t> bytes) {
  if (closed_ || bytes.size() > kMaxChunk) return false;
  uint8_t header[5];
  header[0] = static_cast<uint8_t>(ch);
  const auto n = static_cast<uint32_t>(bytes.size());
  for (int i = 0; i < 4; ++i) header[1 + i] = static_cast<uint8_t>(n >> (8 * i));
  std::lock_guard<std::mutex> lock(send_mu_);
  const int fd = fd_.load();
  if (fd < 0) {
    if (pending_.size() + sizeof(header) + bytes.size() > kMaxPending) return false;
    pending_.insert(pending_.end(), header, header + sizeof(header));
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    return true;
  }
  return WriteAll(fd, header, sizeof(header)) && WriteAll(fd, bytes.data(), bytes.size());
}

void TcpTransport::SetReceiver(ReceiveFn fn) {
  {
    std::lock_guard<std::mutex> lock(recv_mu_);
    receiver_ = std::move(fn);
  }
  recv_cv_.notify_all();
}

void TcpTransport::Close() {
  {
    std::lock_guard<std::mutex> lock(fd_mu_);
    if (!closed_.exchange(true) && fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
  }
  recv_cv_.notify_all();
}

void TcpTransport::ReadLoop() {
  std::vector<uint8_t> buf;
  for (;;) {
    uint8_t header[5];
    if (!ReadAll(fd_, header, sizeof(header))) break;
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= static_cast<uint32_t>(header[1 + i]) << (8 * i);
    if (n > kMaxChunk || header[0] >= kChannelCount) break;
    buf.resize(n);
    if (!ReadAll(fd_, buf.data(), n)) break;
    ReceiveFn fn;
    {
      // Hold data until a receiver is installed so nothing sent right after
      // connecting (e.g. Hello) is lost.
      std::unique_lock<std::mutex> lock(recv_mu_);
      recv_cv_.wait(lock, [&] { return receiver_ || closed_.load(); });
      if (!receiver_) break;
      fn = receiver_;
    }
    fn(static_cast<Channel>(header[0]), buf);
  }
  closed_ = true;
}

}  // namespace rdacore::transport
