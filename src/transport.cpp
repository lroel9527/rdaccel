#include "rdaccel/transport.h"

#include "transport/local_pipe.h"
#include "transport/tcp_transport.h"

namespace rdaccel {

std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>> CreateLocalPipe() {
  auto [a, b] = transport::MakeLocalPipe();
  return {std::move(a), std::move(b)};
}

std::unique_ptr<Transport> ListenTcp(uint16_t port, std::string* error) {
  return transport::TcpTransport::Listen(port, error);
}

std::unique_ptr<Transport> ConnectTcp(const std::string& host, uint16_t port, std::string* error) {
  return transport::TcpTransport::Connect(host, port, error);
}

}  // namespace rdaccel
