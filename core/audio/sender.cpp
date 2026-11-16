#include "audio/sender.h"

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace rdacore::audio {
namespace {

constexpr int kTailMs = 200;      // silence before the gate closes
constexpr int kMonoAfterMs = 500;  // left = right this long: mono
constexpr int kSpurtExtras = 5;    // packets of a spurt with CLOCK (and RED after the first)
constexpr int kClockEvery = 250;   // packets
constexpr double kRedOn = 0.01, kRedOff = 0.003;
constexpr uint64_t kRedOffAfterUs = 30'000'000;
// A gap in the input longer than this is silence (sources call back every
// ~10 ms; a scheduling hiccup is not a gap).
constexpr uint64_t kGapUs = 60'000;
// Silence written out in full after a gap (enough for the gate to close);
// the rest only moves the sample positions on.
constexpr size_t kGapFillSamples = kSampleRate * 3 / 10;
// A slow path (5.5): the sound on the wire keeps within a quarter of the
// estimated bandwidth, down to 32 kbps; once the estimate allows, one step up
// every 5 s.
constexpr int kMinPathBitrate = 32'000;
constexpr int kPathSteps[] = {32'000, 48'000, 64'000, 96'000};
constexpr uint64_t kPathRaiseUs = 5'000'000;
// Per packet on the wire besides the Opus data: the sound header, the UDP
// transport's header, the encryption, UDP and IPv4 (docs/audio.md, 11.2).
constexpr int kWireOverheadBytes = 9 + 56;

bool Silent(const float* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (std::fabs(p[i]) >= kSilenceThreshold) return false;
  }
  return true;
}

bool LeftIsRight(const float* p, size_t frames) {
  for (size_t i = 0; i < frames; ++i) {
    if (p[2 * i] != p[2 * i + 1]) return false;
  }
  return true;
}

}  // namespace

int ProfileBitrate(int profile, int channels) {
  if (profile == kProfileSaver) return channels == 1 ? 56'000 : 96'000;
  return channels == 1 ? 96'000 : 160'000;
}

int ProfileFrameSamples(int profile) { return profile == kProfileSaver ? 960 : 480; }

Sender::Sender(SendFn send, int profile) : send_(std::move(send)), profile_(profile) {
  int err = 0;
  enc_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
  if (!enc_ || err != OPUS_OK) throw std::runtime_error("cannot create the Opus encoder");
  opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
  opus_encoder_ctl(enc_, OPUS_SET_VBR_CONSTRAINT(0));
  opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(10));
  opus_encoder_ctl(enc_, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
  opus_encoder_ctl(enc_, OPUS_SET_LSB_DEPTH(24));
  opus_encoder_ctl(enc_, OPUS_SET_DTX(0));
  opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(0));
  frame_samples_ = ProfileFrameSamples(profile);
  frame_.assign(static_cast<size_t>(frame_samples_) * kChannels, 0.0f);
  prev_.assign(frame_.size(), 0.0f);
  opus_.resize(kMaxOpusFrame);
  red_opus_.resize(kMaxOpusFrame);
  Configure(2);
  status_frame_ms_ = frame_samples_ / 48;
}

Sender::~Sender() {
  if (enc_) opus_encoder_destroy(enc_);
}

void Sender::SetProfile(int profile) { profile_ = profile == kProfileSaver ? kProfileSaver : kProfileFull; }
void Sender::SetPaused(bool paused) { paused_ = paused; }
void Sender::SetPath(double loss_fraction, int64_t bandwidth_bps) {
  loss_ = std::clamp(loss_fraction, 0.0, 1.0);
  bandwidth_ = std::max<int64_t>(0, bandwidth_bps);
}

SenderStats Sender::stats() const {
  SenderStats s;
  s.packets = packets_;
  s.bytes = bytes_;
  s.frames = frames_;
  s.frames_gated = frames_gated_;
  s.red_bytes = red_bytes_;
  return s;
}

SenderStatus Sender::status() const {
  SenderStatus s;
  s.bitrate_bps = status_bitrate_;
  s.frame_ms = status_frame_ms_;
  s.channels = status_channels_;
  s.silent = status_silent_;
  return s;
}

void Sender::Configure(int channels) {
  int bitrate = ProfileBitrate(profile_, channels);
  if (path_cap_bps_ > 0) bitrate = std::min(bitrate, path_cap_bps_);
  if (channels != applied_channels_) {
    opus_encoder_ctl(enc_, OPUS_SET_FORCE_CHANNELS(channels));
    applied_channels_ = channels;
  }
  if (bitrate != applied_bitrate_) {
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate));
    applied_bitrate_ = bitrate;
  }
  const int loss = std::clamp(static_cast<int>(std::lround(loss_.load() * 100)), 0, 20);
  if (loss != applied_loss_) {
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(loss));
    applied_loss_ = loss;
  }
  status_bitrate_ = bitrate;
  status_channels_ = channels;
}

int Sender::Encode(const float* pcm, uint8_t* out) {
  return opus_encode_float(enc_, pcm, frame_samples_, out, static_cast<opus_int32>(kMaxOpusFrame));
}

void Sender::Push(const float* pcm, size_t frames, uint64_t time_us) {
  // A gap: silence for that long.
  if (next_time_ != 0 && time_us > next_time_ + kGapUs) {
    const auto gap = static_cast<size_t>((time_us - next_time_) * kSampleRate / 1'000'000);
    static const std::vector<float> zeros(static_cast<size_t>(kSampleRate / 100) * kChannels, 0.0f);
    size_t left = gap;
    uint64_t t = next_time_;
    // In full until the gate has surely closed, then whole frames at once.
    while (left > 0 && (gap - left < kGapFillSamples || fill_ != 0)) {
      const size_t n = std::min(left, zeros.size() / kChannels);
      next_time_ = 0;  // no gap within the gap
      Push(zeros.data(), n, t);
      t += n * 1'000'000 / kSampleRate;
      left -= n;
    }
    const size_t skip = left / static_cast<size_t>(frame_samples_);
    ts_ += static_cast<uint32_t>(skip * static_cast<size_t>(frame_samples_));
    frames_gated_ += skip;
    left -= skip * static_cast<size_t>(frame_samples_);
    if (left > 0) {
      next_time_ = 0;
      Push(zeros.data(), left, time_us - left * 1'000'000 / kSampleRate);
    }
  }
  next_time_ = time_us + frames * 1'000'000 / kSampleRate;
  size_t done = 0;
  while (done < frames) {
    if (fill_ == 0) frame_time_ = time_us + done * 1'000'000 / kSampleRate;
    const size_t n = std::min(frames - done, static_cast<size_t>(frame_samples_) - fill_);
    std::memcpy(frame_.data() + fill_ * kChannels, pcm + done * kChannels, n * kChannels * sizeof(float));
    fill_ += n;
    done += n;
    if (fill_ == static_cast<size_t>(frame_samples_)) {
      Frame(frame_.data(), frame_time_);
      fill_ = 0;
      // A profile change takes effect between frames.
      const int want = ProfileFrameSamples(profile_);
      if (want != frame_samples_) {
        frame_samples_ = want;
        frame_.assign(static_cast<size_t>(want) * kChannels, 0.0f);
        prev_.assign(frame_.size(), 0.0f);
        have_prev_ = false;
        last_opus_valid_ = false;  // RED copies only frames of the same length
        status_frame_ms_ = want / 48;
      }
    }
  }
}

void Sender::AdaptToPath() {
  // A slow path: what the sound needs on the wire against a quarter of the
  // estimate (none: as the profile says; it is 0 until the transport trusts
  // its estimate, and TCP has none).
  if (const int64_t bw = bandwidth_; bw > 0) {
    const int top = ProfileBitrate(profile_, 2);
    const int overhead = kWireOverheadBytes * 8 * (1000 / std::max(1, frame_samples_ / 48));
    const auto budget = static_cast<int>(std::min<int64_t>(bw / 4 - overhead, top));
    const int now_bps = path_cap_bps_ > 0 ? path_cap_bps_ : top;
    if (now_bps > budget) {
      // Down at once, as far as needed.
      path_cap_bps_ = std::max(kMinPathBitrate, budget / 1000 * 1000);
      if (path_cap_bps_ >= top) path_cap_bps_ = 0;
      path_raise_time_ = frame_time_;
    } else if (path_cap_bps_ > 0 && frame_time_ - path_raise_time_ >= kPathRaiseUs) {
      // Up a step at a time while the estimate allows it.
      int next = top;
      for (int step : kPathSteps) {
        if (step > path_cap_bps_) {
          next = step;
          break;
        }
      }
      if (next <= budget) {
        path_cap_bps_ = next >= top ? 0 : next;
        path_raise_time_ = frame_time_;
      }
    }
  }
  // RED while the path loses packets.
  const double loss = loss_;
  if (loss >= kRedOn) {
    red_on_ = true;
    low_loss_since_ = 0;
  } else if (red_on_) {
    if (loss >= kRedOff) {
      low_loss_since_ = 0;
    } else if (low_loss_since_ == 0) {
      low_loss_since_ = frame_time_ ? frame_time_ : 1;
    } else if (frame_time_ - low_loss_since_ >= kRedOffAfterUs) {
      red_on_ = false;
      low_loss_since_ = 0;
    }
  }
}

void Sender::Frame(const float* pcm, uint64_t time_us) {
  const size_t n = static_cast<size_t>(frame_samples_);
  const int frame_ms = frame_samples_ / 48;
  const uint32_t ts = ts_;
  ts_ += static_cast<uint32_t>(n);
  if (path_checked_ == 0 || time_us - path_checked_ >= 1'000'000 || time_us < path_checked_) {
    path_checked_ = time_us;
    AdaptToPath();
  }
  const bool silent = Silent(pcm, n * kChannels);
  const bool same = LeftIsRight(pcm, n);
  // Only sound counts towards mono: a spurt starts in stereo (at 10 ms,
  // Opus's forced mono softens the first two milliseconds of an attack).
  same_ms_ = same && sending_ ? same_ms_ + frame_ms : 0;
  const bool mono = same && same_ms_ >= kMonoAfterMs;

  auto keep_as_prev = [&] {
    std::memcpy(prev_.data(), pcm, n * kChannels * sizeof(float));
    have_prev_ = true;
  };

  if (paused_) {
    // Nobody hears it: nothing is encoded; what comes after starts afresh.
    if (sending_) sending_ = false;
    was_paused_ = true;
    status_silent_ = true;
    last_opus_valid_ = false;
    keep_as_prev();
    return;
  }
  if (!sending_) {
    if (silent && !was_paused_) {
      ++frames_gated_;
      last_opus_valid_ = false;
      keep_as_prev();
      return;
    }
    if (silent) {  // just unpaused, and silent: the gate is closed
      was_paused_ = false;
      ++frames_gated_;
      keep_as_prev();
      return;
    }
    // Sound again: a new spurt, from a fresh encoder state, with the frame
    // before as its lead-in.
    was_paused_ = false;
    sending_ = true;
    silent_ms_ = 0;
    ++spurt_;
    spurt_packets_ = 0;
    since_clock_ = 0;
    opus_encoder_ctl(enc_, OPUS_RESET_STATE);
    applied_channels_ = applied_bitrate_ = 0;
    applied_loss_ = -1;
    mono_ = mono;
    Configure(mono ? 1 : 2);
    last_opus_valid_ = false;
    if (have_prev_) {
      const int r = Encode(prev_.data(), red_opus_.data());
      if (r > 0) {
        last_opus_.assign(red_opus_.begin(), red_opus_.begin() + r);
        last_opus_valid_ = true;
      }
    }
  } else {
    silent_ms_ = silent ? silent_ms_ + frame_ms : 0;
  }
  if (mono != mono_) mono_ = mono;
  Configure(mono_ ? 1 : 2);
  const int r = Encode(pcm, opus_.data());
  keep_as_prev();
  if (r <= 0) {
    last_opus_valid_ = false;
    return;
  }
  ++frames_;
  const bool end = silent_ms_ >= kTailMs;
  const bool red = last_opus_valid_ && (red_on_ || spurt_packets_ < kSpurtExtras);
  const bool clock = spurt_packets_ < kSpurtExtras || since_clock_ >= kClockEvery;
  Packet p;
  p.flags = static_cast<uint8_t>((clock ? kFlagClock : 0) | (red ? kFlagRed : 0) | (end ? kFlagEnd : 0));
  p.spurt = spurt_;
  p.seq = seq_++;
  p.ts = ts;
  p.time_us = time_us;
  if (red) p.red = last_opus_;
  p.main = std::span<const uint8_t>(opus_.data(), static_cast<size_t>(r));
  if (EncodePacket(p, packet_)) {
    send_(packet_);
    ++packets_;
    bytes_ += packet_.size();
    if (red) red_bytes_ += 2 + last_opus_.size();
  }
  ++spurt_packets_;
  since_clock_ = clock ? 0 : since_clock_ + 1;
  last_opus_.assign(opus_.begin(), opus_.begin() + r);
  last_opus_valid_ = true;
  status_silent_ = false;
  if (end) {
    sending_ = false;
    status_silent_ = true;
    last_opus_valid_ = false;
  }
}

}  // namespace rdacore::audio
