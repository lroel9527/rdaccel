#include "rdacore/transport.h"

#include "net/reliable_transport.h"
#include "transport/local_pipe.h"
#include "transport/tcp_transport.h"

namespace rdacore {

std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>> CreateLocalPipe() {
  auto [a, b] = transport::MakeLocalPipe();
  return {std::move(a), std::move(b)};
}

std::unique_ptr<Transport> BindTcp(uint16_t port, std::string* error) {
  return transport::TcpTransport::Bind(port, error);
}

std::unique_ptr<Transport> ConnectTcp(const std::string& host, uint16_t port, std::string* error) {
  return transport::TcpTransport::Connect(host, port, error);
}

std::unique_ptr<Transport> BindUdp(uint16_t port, std::string* error) {
  auto sock = net::BindUdpSocket(port, error);
  if (!sock) return nullptr;
  return std::make_unique<net::ReliableTransport>(std::move(sock));
}

std::unique_ptr<Transport> UdpFromFd(int fd, bool viewer_side, std::string* error) {
  auto sock = net::UdpSocketFromFd(fd, error);
  if (!sock) return nullptr;
  return std::make_unique<net::ReliableTransport>(std::move(sock),
                                                  net::ReliableOptions{.client = viewer_side});
}

std::unique_ptr<Transport> ConnectUdp(const std::string& host, uint16_t port, std::string* error) {
  auto sock = net::ConnectUdpSocket(host, port, error);
  if (!sock) return nullptr;
  return std::make_unique<net::ReliableTransport>(std::move(sock), net::ReliableOptions{.client = true});
}

namespace {
net::LinkConditions ToLink(const NetworkConditions& c) {
  net::LinkConditions l;
  l.bandwidth_bps = c.bandwidth_bps;
  l.delay_ms = c.delay_ms;
  l.jitter_ms = c.jitter_ms;
  l.loss = c.loss;
  l.queue_ms = c.queue_ms;
  l.bandwidth_schedule = c.bandwidth_schedule;
  l.seed = c.seed;
  return l;
}
}  // namespace

std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>> CreateSimulatedNetwork(
    const NetworkConditions& host_to_viewer, const NetworkConditions& viewer_to_host,
    bool congestion_control) {
  auto [a, b] = net::MakeSimulatedLink(ToLink(host_to_viewer), ToLink(viewer_to_host));
  return {std::make_unique<net::ReliableTransport>(
              std::move(a), net::ReliableOptions{.client = false, .congestion_control = congestion_control}),
          std::make_unique<net::ReliableTransport>(
              std::move(b), net::ReliableOptions{.client = true, .congestion_control = congestion_control})};
}

}  // namespace rdacore
