#include "session/link.h"

#include <algorithm>
#include <chrono>

#include "capture/capturer.h"
#include "log/log.h"

namespace rdacore::session {
namespace {

// AppMessages go out in pieces so one large message does not need one large
// transport write (TCP and custom transports may cap those).
constexpr size_t kMessageChunk = 256 * 1024;

uint64_t Now() { return capture::MonotonicMicros(); }

}  // namespace

const char* LinkErrorText(LinkError e) {
  switch (e) {
    case LinkError::kNone: return "no error";
    case LinkError::kClosed: return "connection closed";
    case LinkError::kTimeout: return "peer timed out";
    case LinkError::kProtocol: return "incompatible peer or malformed data";
    case LinkError::kRejected: return "rejected by the host";
  }
  return "unknown";
}

Link::Link(Role role, transport::ITransport* transport, LinkOptions options, Handlers handlers)
    : role_(role), transport_(transport), options_(std::move(options)), h_(std::move(handlers)) {}

Link::~Link() { Shutdown(); }

void Link::Shutdown() {
  if (quiet_.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(timer_mu_);
    stop_ = true;
  }
  timer_cv_.notify_all();
  if (timer_.joinable()) timer_.join();
  const LinkState s = state_.load();
  if (s != LinkState::kDisconnected && (role_ == Role::kViewer || s == LinkState::kConnected)) {
    SendControl(proto::Goodbye{proto::GoodbyeReason::kClosed});
  }
}

void Link::Start() {
  last_rx_us_ = Now();
  if (role_ == Role::kViewer) {
    proto::ViewerHello hello;
    hello.auth_token = options_.auth_token;
    SendControl(hello);
  }
  timer_ = std::thread([this] { Timer(); });
}

LinkError Link::error() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return error_;
}

void Link::SetState(LinkState s, LinkError e) {
  // cb_mu_ keeps callbacks in transition order; it is recursive so a handler
  // may call back into the link (SendMessage, Close) on the same thread.
  std::lock_guard<std::recursive_mutex> cb(cb_mu_);
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    const LinkState cur = state_.load();
    if (cur == LinkState::kDisconnected || cur == s) return;
    state_ = s;
    error_ = e;
  }
  if (s == LinkState::kDisconnected) {
    log::Info(std::string(role_ == Role::kHost ? "host" : "viewer") + " disconnected: " + LinkErrorText(e));
  } else if (s == LinkState::kConnected) {
    log::Info(std::string(role_ == Role::kHost ? "host" : "viewer") + " connected");
  }
  if (h_.on_state && !quiet_) h_.on_state(s, e);
}

void Link::Fail(LinkError e, proto::GoodbyeReason why) {
  if (disconnected()) return;
  SendControl(proto::Goodbye{why});
  SetState(LinkState::kDisconnected, e);
}

void Link::Close() {
  if (disconnected()) return;
  if (role_ == Role::kViewer || state_.load() == LinkState::kConnected) {
    SendControl(proto::Goodbye{proto::GoodbyeReason::kClosed});
  }
  SetState(LinkState::kDisconnected, LinkError::kClosed);
}

bool Link::SendControl(const proto::Message& m) {
  return transport_ && transport_->Send(transport::Channel::kControl, proto::Encode(m));
}

void Link::OnReceive(transport::Channel ch, std::span<const uint8_t> bytes) {
  last_rx_us_ = Now();
  if (disconnected() || quiet_) return;
  switch (ch) {
    case transport::Channel::kControl:
      control_parser_.Append(bytes);
      while (auto m = control_parser_.Next()) {
        HandleControl(std::move(*m));
        if (disconnected()) return;
      }
      if (!control_parser_.error().empty()) {
        log::Warn("control channel: " + control_parser_.error());
        Fail(LinkError::kProtocol, proto::GoodbyeReason::kIncompatible);
        return;
      }
      if (role_ == Role::kViewer && h_.on_raw) h_.on_raw(ch, bytes);
      return;
    case transport::Channel::kInput:
      // The host takes nothing but the handshake from a viewer it has not
      // accepted (the auth hook must not be bypassed).
      if (role_ != Role::kHost || state_.load() != LinkState::kConnected) return;
      input_parser_.Append(bytes);
      while (auto m = input_parser_.Next()) {
        if (h_.on_other) h_.on_other(*m);
      }
      if (!input_parser_.error().empty()) {
        log::Warn("input channel: " + input_parser_.error());
        Fail(LinkError::kProtocol, proto::GoodbyeReason::kIncompatible);
      }
      return;
    case transport::Channel::kMessage:
      if (role_ == Role::kHost && state_.load() != LinkState::kConnected) return;
      message_parser_.Append(bytes);
      while (auto m = message_parser_.Next()) {
        if (auto* am = std::get_if<proto::AppMessage>(&*m); am && h_.on_message) {
          h_.on_message(am->type, am->payload);
        }
      }
      if (!message_parser_.error().empty()) {
        log::Warn("message channel: " + message_parser_.error());
        Fail(LinkError::kProtocol, proto::GoodbyeReason::kIncompatible);
      }
      return;
    default:
      if (role_ == Role::kViewer && h_.on_raw) h_.on_raw(ch, bytes);
      return;
  }
}

void Link::HandleControl(proto::Message&& m) {
  // Viewer: the host's Welcome (or, from a version 3 host, its Hello).
  const proto::Welcome* welcome = std::get_if<proto::Welcome>(&m);
  const proto::Hello* hello = std::get_if<proto::Hello>(&m);
  if (welcome || hello) {
    if (role_ != Role::kViewer || state_.load() != LinkState::kConnecting) return;
    const uint32_t version = welcome ? welcome->version : hello->version;
    const uint32_t min_version = welcome ? welcome->min_version : hello->min_version;
    if (version < proto::kMinHostVersion || min_version > proto::kProtocolVersion) {
      log::Warn("host speaks protocol " + std::to_string(version) + " (needs viewers >= " +
                std::to_string(min_version) + "), this viewer " + std::to_string(proto::kProtocolVersion));
      Fail(LinkError::kProtocol, proto::GoodbyeReason::kIncompatible);
      return;
    }
    peer_caps_ = welcome ? welcome->caps : hello->caps;
    SetState(LinkState::kConnected, LinkError::kNone);
    return;
  }
  if (auto* vh = std::get_if<proto::ViewerHello>(&m)) {
    if (role_ != Role::kHost) return;
    if (vh->version < proto::kMinViewerVersion || vh->min_version > proto::kProtocolVersion) {
      log::Warn("viewer speaks protocol " + std::to_string(vh->version) + ", this host " +
                std::to_string(proto::kProtocolVersion));
      Fail(LinkError::kProtocol, proto::GoodbyeReason::kIncompatible);
      return;
    }
    if (state_.load() != LinkState::kConnecting) return;  // one viewer per host
    if (h_.on_auth && !h_.on_auth(vh->auth_token)) {
      Fail(LinkError::kRejected, proto::GoodbyeReason::kRejected);
      return;
    }
    peer_caps_ = vh->caps;
    // Connected before the Welcome goes out, so whatever the viewer sends
    // once it has the Welcome is accepted.
    SetState(LinkState::kConnected, LinkError::kNone);
    SendControl(proto::Welcome{});
    return;
  }
  if (std::holds_alternative<proto::Ping>(m)) return;
  if (auto* bye = std::get_if<proto::Goodbye>(&m)) {
    LinkError e = LinkError::kClosed;
    if (bye->reason == proto::GoodbyeReason::kIncompatible) e = LinkError::kProtocol;
    if (bye->reason == proto::GoodbyeReason::kRejected) e = LinkError::kRejected;
    SetState(LinkState::kDisconnected, e);
    return;
  }
  if (role_ == Role::kHost && state_.load() == LinkState::kConnected && h_.on_other) h_.on_other(m);
}

bool Link::SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error) {
  if (disconnected()) {
    if (error) *error = std::string("disconnected: ") + LinkErrorText(this->error());
    return false;
  }
  if (state_.load() != LinkState::kConnected) {
    if (error) *error = "not connected yet";
    return false;
  }
  if (type == 0 || (type < kFirstUserMessage && type != kClipboardTextMessage)) {
    if (error) *error = "message type " + std::to_string(type) + " is reserved (use 0x10000 and above)";
    return false;
  }
  if (payload.size() > proto::kMaxAppMessage) {
    if (error) *error = "message larger than 16 MiB";
    return false;
  }
  const auto bytes = proto::Encode(proto::AppMessage{type, {payload.begin(), payload.end()}});
  bool ok = true;
  {
    std::lock_guard<std::mutex> lock(send_mu_);
    for (size_t off = 0; ok && off < bytes.size(); off += kMessageChunk) {
      const size_t n = std::min(kMessageChunk, bytes.size() - off);
      ok = transport_->Send(transport::Channel::kMessage, std::span<const uint8_t>(bytes).subspan(off, n));
    }
  }
  if (!ok) {
    if (error) *error = "connection closed";
    SetState(LinkState::kDisconnected, LinkError::kClosed);
  }
  return ok;
}

void Link::Timer() {
  // Several pings per timeout, so a short timeout does not fire between them.
  int ping_ms = options_.ping_interval_ms;
  if (options_.peer_timeout_ms > 0) ping_ms = std::min(ping_ms, options_.peer_timeout_ms / 4);
  const auto period = std::chrono::milliseconds(std::max(10, ping_ms));
  std::unique_lock<std::mutex> lock(timer_mu_);
  while (!timer_cv_.wait_for(lock, period, [this] { return stop_; })) {
    lock.unlock();
    const LinkState s = state_.load();
    if (s != LinkState::kDisconnected) {
      // The host pings only once a viewer is there (not into a recording).
      if (role_ == Role::kViewer || s == LinkState::kConnected) {
        if (!SendControl(proto::Ping{Now()})) SetState(LinkState::kDisconnected, LinkError::kClosed);
      }
      const uint64_t quiet_us = Now() - last_rx_us_.load();
      if (options_.peer_timeout_ms > 0 && (role_ == Role::kViewer || s == LinkState::kConnected) &&
          quiet_us > static_cast<uint64_t>(options_.peer_timeout_ms) * 1000) {
        Fail(LinkError::kTimeout, proto::GoodbyeReason::kClosed);
      }
    }
    lock.lock();
  }
}

}  // namespace rdacore::session
