#include "transport/secure_transport.h"

#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "monocypher.h"

namespace rdacore::transport {
namespace {

constexpr uint8_t kMagic[4] = {'R', 'D', 'A', 'S'};
constexpr uint8_t kVersion = 1;
constexpr size_t kHelloSize = sizeof(kMagic) + 1 + 32;
constexpr size_t kTagSize = 16;
constexpr size_t kMaxRecord = 64 * 1024;  // plaintext bytes per record
constexpr char kLabel[] = "rdaccel secure v1";

void Random(uint8_t* out, size_t n) {
  while (n > 0) {
    const size_t k = std::min<size_t>(n, 256);  // getentropy's limit
    if (::getentropy(out, k) != 0) throw std::runtime_error("no system randomness (getentropy failed)");
    out += k;
    n -= k;
  }
}

void Nonce(size_t ch, uint64_t counter, uint8_t nonce[24]) {
  std::memset(nonce, 0, 24);
  nonce[0] = static_cast<uint8_t>(ch);
  for (int i = 0; i < 8; ++i) nonce[16 + i] = static_cast<uint8_t>(counter >> (8 * i));
}

}  // namespace

SecureTransport::SecureTransport(std::unique_ptr<ITransport> inner, bool is_host,
                                 std::span<const uint8_t, kSecureKeySize> key)
    : inner_(std::move(inner)), is_host_(is_host) {
  std::copy(key.begin(), key.end(), app_key_.begin());
  Random(my_random_.data(), my_random_.size());
  // The hello goes first on the control channel, before anything the
  // session sends (which waits in pending_ until the keys are agreed).
  std::vector<uint8_t> hello(kMagic, kMagic + sizeof(kMagic));
  hello.push_back(kVersion);
  hello.insert(hello.end(), my_random_.begin(), my_random_.end());
  inner_->Send(Channel::kControl, hello);
}

SecureTransport::~SecureTransport() {
  inner_.reset();  // stops the receive thread first
  crypto_wipe(app_key_.data(), app_key_.size());
  crypto_wipe(tx_key_.data(), tx_key_.size());
  crypto_wipe(rx_key_.data(), rx_key_.size());
}

void SecureTransport::SetReceiver(ReceiveFn fn) {
  bool start = false;
  {
    std::lock_guard<std::mutex> lock(recv_mu_);
    receiver_ = std::move(fn);
    start = !receiving_;
    receiving_ = true;
  }
  // The inner transport holds what arrives before this.
  if (start) inner_->SetReceiver([this](Channel ch, std::span<const uint8_t> b) { OnInner(ch, b); });
}

void SecureTransport::Fail(ConnectionError e, const std::string& msg, bool tell_peer) {
  if (failed_) return;
  failed_ = true;
  for (auto& b : rx_) b.clear();
  if (tell_peer) {
    // An alert, in clear (a zero-length record and a reason byte), so the
    // peer stops at once instead of timing out. Unauthenticated, so it can
    // only end a connection, which anyone on the path can do anyway.
    const uint8_t alert[5] = {0, 0, 0, 0, static_cast<uint8_t>(e)};
    inner_->Send(Channel::kControl, alert);
  }
  if (on_error_) on_error_(e, msg);
}

void SecureTransport::DeriveKeys(std::span<const uint8_t> peer_random) {
  std::vector<uint8_t> msg(kLabel, kLabel + sizeof(kLabel) - 1);
  const auto& host_random = is_host_ ? std::span<const uint8_t>(my_random_) : peer_random;
  const auto& viewer_random = is_host_ ? peer_random : std::span<const uint8_t>(my_random_);
  msg.insert(msg.end(), host_random.begin(), host_random.end());
  msg.insert(msg.end(), viewer_random.begin(), viewer_random.end());
  uint8_t out[64];
  crypto_blake2b_keyed(out, sizeof(out), app_key_.data(), app_key_.size(), msg.data(), msg.size());
  const uint8_t* h2v = out;
  const uint8_t* v2h = out + 32;
  std::memcpy(tx_key_.data(), is_host_ ? h2v : v2h, 32);
  std::memcpy(rx_key_.data(), is_host_ ? v2h : h2v, 32);
  crypto_wipe(out, sizeof(out));
  // Keys first, then each channel opens and sends what waited.
  for (size_t ch = 0; ch < kChannelCount; ++ch) {
    std::lock_guard<std::mutex> lock(tx_mu_[ch]);
    tx_ready_[ch] = true;
    if (!pending_[ch].empty()) SendSealed(ch, pending_[ch]);
    pending_[ch].clear();
    pending_[ch].shrink_to_fit();
  }
  ready_ = true;
}

bool SecureTransport::Send(Channel ch, std::span<const uint8_t> bytes) {
  const auto c = static_cast<size_t>(ch);
  if (c >= kChannelCount) return false;
  std::lock_guard<std::mutex> lock(tx_mu_[c]);
  if (!tx_ready_[c]) {
    // Bounded: the host holds frames while PeerKnown() is false, so only
    // the session's first messages wait here.
    if (pending_[c].size() + bytes.size() > (64u << 20)) return false;
    pending_[c].insert(pending_[c].end(), bytes.begin(), bytes.end());
    return true;
  }
  return SendSealed(c, bytes);
}

bool SecureTransport::SendSealed(size_t ch, std::span<const uint8_t> bytes) {
  std::vector<uint8_t>& out = sealed_[ch];
  const uint8_t ad = static_cast<uint8_t>(ch);
  for (size_t off = 0; off < bytes.size(); off += kMaxRecord) {
    const size_t n = std::min(kMaxRecord, bytes.size() - off);
    out.resize(4 + n + kTagSize);
    out[0] = static_cast<uint8_t>(n >> 24);
    out[1] = static_cast<uint8_t>(n >> 16);
    out[2] = static_cast<uint8_t>(n >> 8);
    out[3] = static_cast<uint8_t>(n);
    uint8_t nonce[24];
    Nonce(ch, tx_count_[ch]++, nonce);
    crypto_aead_lock(out.data() + 4, out.data() + 4 + n, tx_key_.data(), nonce, &ad, 1, bytes.data() + off, n);
    if (!inner_->Send(static_cast<Channel>(ch), out)) return false;
  }
  return true;
}

void SecureTransport::OnInner(Channel ch, std::span<const uint8_t> bytes) {
  const auto c = static_cast<size_t>(ch);
  if (failed_ || c >= kChannelCount) return;
  if (!have_peer_hello_) {
    if (ch != Channel::kControl) {
      // Data the peer sent after its hello can overtake it on another channel.
      rx_[c].insert(rx_[c].end(), bytes.begin(), bytes.end());
      return;
    }
    const size_t take = std::min(kHelloSize - hello_.size(), bytes.size());
    hello_.insert(hello_.end(), bytes.begin(), bytes.begin() + take);
    bytes = bytes.subspan(take);
    const size_t check = std::min(hello_.size(), sizeof(kMagic));
    if (check > 0 && std::memcmp(hello_.data(), kMagic, check) != 0) {
      Fail(ConnectionError::kProtocol, "the peer does not encrypt (set the same key on both sides)");
      return;
    }
    if (hello_.size() < kHelloSize) return;
    if (hello_[4] != kVersion) {
      Fail(ConnectionError::kProtocol, "the peer uses another encryption version");
      return;
    }
    have_peer_hello_ = true;
    DeriveKeys(std::span<const uint8_t>(hello_).subspan(5, 32));
    rx_[c].insert(rx_[c].end(), bytes.begin(), bytes.end());
    for (size_t i = 0; i < kChannelCount; ++i) {
      if (!Drain(i)) return;
    }
    return;
  }
  rx_[c].insert(rx_[c].end(), bytes.begin(), bytes.end());
  Drain(c);
}

bool SecureTransport::Drain(size_t ch) {
  std::vector<uint8_t>& buf = rx_[ch];
  size_t pos = 0;
  const uint8_t ad = static_cast<uint8_t>(ch);
  while (buf.size() - pos >= 4) {
    const size_t n = (static_cast<size_t>(buf[pos]) << 24) | (static_cast<size_t>(buf[pos + 1]) << 16) |
                     (static_cast<size_t>(buf[pos + 2]) << 8) | buf[pos + 3];
    if (n == 0) {
      if (buf.size() - pos < 5) break;
      Fail(ConnectionError::kRejected, "the peer could not authenticate our data (different keys)", false);
      return false;
    }
    if (n > kMaxRecord) {
      Fail(ConnectionError::kRejected, "bad encrypted record (wrong key or tampered data)");
      return false;
    }
    if (buf.size() - pos < 4 + n + kTagSize) break;
    plain_.resize(n);
    uint8_t nonce[24];
    Nonce(ch, rx_count_[ch], nonce);
    if (crypto_aead_unlock(plain_.data(), buf.data() + pos + 4 + n, rx_key_.data(), nonce, &ad, 1,
                           buf.data() + pos + 4, n) != 0) {
      Fail(ConnectionError::kRejected, "record failed authentication (wrong key or tampered data)");
      return false;
    }
    ++rx_count_[ch];
    pos += 4 + n + kTagSize;
    ReceiveFn fn;
    {
      std::lock_guard<std::mutex> lock(recv_mu_);
      fn = receiver_;
    }
    if (fn) fn(static_cast<Channel>(ch), plain_);
  }
  buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(pos));
  return true;
}

}  // namespace rdacore::transport
