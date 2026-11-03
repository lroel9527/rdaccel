#pragma once

// Encryption and authentication for any transport, with a 32-byte key both
// applications share.
//
// Handshake: each side first sends, in clear on the control channel,
//   "RDAS" | version (1) | 32 random bytes
// and both derive per-direction keys
//   BLAKE2b-512(key = app key, "rdaccel secure v1" | host random | viewer random)
//   -> host-to-viewer key (first 32 bytes), viewer-to-host key (last 32)
// so every connection has fresh keys even with a long-lived app key.
//
// After that every channel carries records
//   [ciphertext length: u32 big-endian][ciphertext][Poly1305 tag: 16]
// sealed with XChaCha20-Poly1305; the nonce is the channel number and a
// per-channel, per-direction record counter, the associated data the channel
// number. Records cannot be forged, replayed, reordered or moved between
// channels without the receiver noticing (it then stops: a wrong key or
// tampering).

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "rdacore/connection.h"
#include "transport/transport.h"

namespace rdacore::transport {

inline constexpr size_t kSecureKeySize = 32;

class SecureTransport final : public ITransport {
 public:
  using ErrorFn = std::function<void(ConnectionError, const std::string&)>;

  // `is_host`: which direction key this side sends with.
  SecureTransport(std::unique_ptr<ITransport> inner, bool is_host,
                  std::span<const uint8_t, kSecureKeySize> key);
  ~SecureTransport() override;

  // Called once (on the receive thread) when the peer does not encrypt
  // (kProtocol) or a record fails authentication (kRejected: wrong key or
  // tampered data). Nothing is delivered after that. Set before SetReceiver.
  void SetErrorHandler(ErrorFn fn) { on_error_ = std::move(fn); }

  bool Send(Channel ch, std::span<const uint8_t> bytes) override;
  void SetReceiver(ReceiveFn fn) override;
  void Close() override { inner_->Close(); }
  bool GetStats(TransportStats* out) const override { return inner_->GetStats(out); }
  // Only once the keys are agreed: until then the host holds frames.
  bool PeerKnown() const override { return ready_.load() && inner_->PeerKnown(); }

 private:
  void OnInner(Channel ch, std::span<const uint8_t> bytes);
  void Fail(ConnectionError e, const std::string& msg, bool tell_peer = true);
  void DeriveKeys(std::span<const uint8_t> peer_random);
  bool Drain(size_t ch);  // decrypts complete records of rx_[ch]
  bool SendSealed(size_t ch, std::span<const uint8_t> bytes);  // under tx_mu_[ch]

  std::unique_ptr<ITransport> inner_;
  const bool is_host_;
  std::array<uint8_t, kSecureKeySize> app_key_{};
  std::array<uint8_t, 32> my_random_{};
  std::array<uint8_t, kSecureKeySize> tx_key_{}, rx_key_{};

  // Receive side: only the inner transport's receive thread.
  std::vector<uint8_t> hello_;  // the peer's hello as it arrives
  bool have_peer_hello_ = false;
  bool failed_ = false;
  std::array<std::vector<uint8_t>, kChannelCount> rx_;
  std::array<uint64_t, kChannelCount> rx_count_{};
  std::vector<uint8_t> plain_;
  ErrorFn on_error_;

  std::mutex recv_mu_;
  ReceiveFn receiver_;
  bool receiving_ = false;

  // Send side: one lock per channel keeps records in counter order.
  std::array<std::mutex, kChannelCount> tx_mu_;
  std::array<bool, kChannelCount> tx_ready_{};
  std::array<std::vector<uint8_t>, kChannelCount> pending_;  // before the keys
  std::array<uint64_t, kChannelCount> tx_count_{};
  std::atomic<bool> ready_{false};
  std::array<std::vector<uint8_t>, kChannelCount> sealed_;  // scratch, under tx_mu_
};

}  // namespace rdacore::transport
