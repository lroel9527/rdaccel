#include "audio/receiver.h"

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "audio/audio_packet.h"
#include "audio/audio_source.h"

namespace rdacore::audio {
namespace {

constexpr double kMinTargetMs = 20, kMaxTargetMs = 60, kUnderrunMaxTargetMs = 200, kStartTargetMs = 30;
constexpr double kJitterMarginMs = 5;
constexpr uint64_t kJitterWindowUs = 5'000'000, kMinWindowUs = 10'000'000;
constexpr uint64_t kUnderrunHoldUs = 30'000'000;
constexpr uint64_t kIdleAfterUs = 100'000;  // no packets: the spurt is over
constexpr int kMaxConcealed = 5;
constexpr uint64_t kOverForUs = 500'000;
constexpr int kCrossfade = 240;  // 5 ms
constexpr size_t kTick = kSampleRate / 10;  // drift control every 100 ms of output
constexpr double kMaxPpm = 5000, kMaxIntegral = 500;
// The largest Opus frame (120 ms).
constexpr int kMaxFrameSamples = 5760;
// A/V: ahead by more than this for kAheadForUs delays the sound.
constexpr double kAheadMs = 20, kMaxAvExtraMs = 120;
constexpr uint64_t kAheadForUs = 2'000'000;

}  // namespace

struct Receiver::Slot {
  uint64_t arrival = 0;
  uint16_t len = 0;
  uint8_t data[kMaxPacket];
};

struct Receiver::Frame {
  bool used = false;
  int64_t ts = 0;
  int spurt = 0;
  int samples = 0;
  uint64_t arrival = 0;
  uint16_t len = 0;
  uint8_t data[kMaxOpusFrame];
};

Receiver::Receiver()
    : ring_(new Slot[kRingSlots]), frames_(new Frame[kFrames]), resampler_(kChannels, 16384) {
  int err = 0;
  dec_ = opus_decoder_create(kSampleRate, kChannels, &err);
  if (!dec_ || err != OPUS_OK) throw std::runtime_error("cannot create the Opus decoder");
  pcm_.assign(static_cast<size_t>(kMaxFrameSamples) * kChannels, 0.0f);
  fade_.assign(static_cast<size_t>(kCrossfade) * kChannels, 0.0f);
  delays_.resize(kDelays);
  scratch_.resize(kDelays);
  video_.resize(256);
}

Receiver::~Receiver() {
  if (dec_) opus_decoder_destroy(dec_);
}

void Receiver::OnPacket(std::span<const uint8_t> bytes, uint64_t arrival_us) {
  if (bytes.size() < kPacketHeader + 1 || bytes.size() > kMaxPacket) return;
  const size_t w = ring_write_.load(std::memory_order_relaxed);
  const size_t r = ring_read_.load(std::memory_order_acquire);
  if (w - r >= kRingSlots) {  // full: Read is not keeping up (or not called)
    ++dropped_;
    return;
  }
  Slot& s = ring_[w % kRingSlots];
  s.arrival = arrival_us;
  s.len = static_cast<uint16_t>(bytes.size());
  std::memcpy(s.data, bytes.data(), bytes.size());
  ring_write_.store(w + 1, std::memory_order_release);
}

ReceiverStats Receiver::stats() const {
  ReceiverStats s;
  s.packets = packets_;
  s.bytes = bytes_;
  s.lost = lost_;
  s.late = late_;
  s.underruns = underruns_;
  s.catchups = catchups_;
  s.dropped = dropped_;
  s.buffer_ms = buffer_ms_;
  s.target_ms = target_ms_out_;
  s.drift_ppm = drift_ppm_;
  s.av_offset_ms = av_offset_ms_;
  return s;
}

void Receiver::NoteFrameShown(uint64_t capture_time_us, uint64_t shown_us) {
  // A spin lock (NoteFrameShown may come from more than one thread; Read
  // only reads the atomics this writes).
  while (video_lock_.exchange(true, std::memory_order_acquire)) {
  }
  video_[video_head_] = {shown_us, static_cast<int64_t>(shown_us) - static_cast<int64_t>(capture_time_us)};
  video_head_ = (video_head_ + 1) % video_.size();
  video_count_ = std::min(video_count_ + 1, video_.size());
  // The median of the last second.
  int64_t vals[256];
  size_t n = 0;
  for (size_t i = 0; i < video_count_; ++i) {
    const VideoSample& v = video_[(video_head_ + video_.size() - 1 - i) % video_.size()];
    if (shown_us - v.time > 1'000'000) break;
    vals[n++] = v.d;
  }
  std::nth_element(vals, vals + n / 2, vals + n);
  video_delay_us_ = vals[n / 2];
  video_last_us_ = shown_us;
  video_lock_.store(false, std::memory_order_release);
}

Receiver::Frame* Receiver::Find(int64_t ts, int spurt) {
  for (size_t i = 0; i < kFrames; ++i) {
    if (frames_[i].used && frames_[i].ts == ts && frames_[i].spurt == spurt) return &frames_[i];
  }
  return nullptr;
}

int64_t Receiver::SpurtEnd(int spurt) const {
  int64_t end = -1;
  for (size_t i = 0; i < kFrames; ++i) {
    const Frame& f = frames_[i];
    if (f.used && f.spurt == spurt) end = std::max(end, f.ts + f.samples);
  }
  return end;
}

void Receiver::Insert(int64_t ts, uint8_t spurt, std::span<const uint8_t> opus, uint64_t arrival, bool copy) {
  if (opus.empty() || opus.size() > kMaxOpusFrame) return;
  const int samples = opus_packet_get_nb_samples(opus.data(), static_cast<opus_int32>(opus.size()), kSampleRate);
  if (samples <= 0 || samples > kMaxFrameSamples) return;
  // Already played or skipped (positions run on through every spurt): late.
  if (played_any_ && ts < play_ts_) {
    if (!copy) ++late_;  // a copy (RED) of what already played is expected
    return;
  }
  Frame* free_slot = nullptr;
  Frame* oldest = nullptr;
  for (size_t i = 0; i < kFrames; ++i) {
    Frame& f = frames_[i];
    if (!f.used) {
      if (!free_slot) free_slot = &f;
      continue;
    }
    if (f.ts == ts && f.spurt == spurt) {  // a duplicate (or RED of one we have)
      if (!copy) ++late_;
      return;
    }
    if (!oldest || f.ts < oldest->ts) oldest = &f;
  }
  if (!free_slot) {
    if (oldest->ts > ts) return;  // older than everything kept
    free_slot = oldest;
  }
  free_slot->used = true;
  free_slot->ts = ts;
  free_slot->spurt = spurt;
  free_slot->samples = samples;
  free_slot->arrival = arrival;
  free_slot->len = static_cast<uint16_t>(opus.size());
  std::memcpy(free_slot->data, opus.data(), opus.size());
}

void Receiver::NoteDelay(int64_t ts, uint64_t arrival) {
  if (!have_first_) {
    have_first_ = true;
    first_arrival_ = arrival;
    first_ts_ = ts;
  }
  const int64_t d = static_cast<int64_t>(arrival - first_arrival_) - (ts - first_ts_) * 1'000'000 / kSampleRate;
  delays_[delays_head_] = {arrival, d};
  delays_head_ = (delays_head_ + 1) % kDelays;
  delays_count_ = std::min(delays_count_ + 1, kDelays);
}

void Receiver::NoteLevel(int samples, uint64_t arrival, uint64_t now) {
  if (samples <= 0) return;
  // The buffer as it was when this packet came (play has gone on since),
  // less half its frame: over the frame it falls from full to empty.
  const double ms = BufferMs() + static_cast<double>(now - std::min(now, arrival)) / 1000.0 -
                    samples * 500.0 / kSampleRate;
  levels_[level_head_] = {arrival, ms};
  level_head_ = (level_head_ + 1) % kLevels;
  level_count_ = std::min(level_count_ + 1, kLevels);
}

void Receiver::Drain(uint64_t now) {
  const size_t w = ring_write_.load(std::memory_order_acquire);
  size_t r = ring_read_.load(std::memory_order_relaxed);
  for (; r != w; ++r) {
    const Slot& s = ring_[r % kRingSlots];
    const auto p = ParsePacket(std::span<const uint8_t>(s.data, s.len));
    if (!p) continue;
    ++packets_;
    bytes_ += s.len;
    // Unwrap ts against the last one.
    int64_t ts = p->ts;
    if (have_ts_) {
      ts = last_ts_ + static_cast<int32_t>(p->ts - static_cast<uint32_t>(last_ts_));
    }
    have_ts_ = true;
    last_ts_ = ts;
    last_arrival_ = std::max(last_arrival_, s.arrival);
    NoteDelay(ts, s.arrival);
    if (p->clock()) {
      have_clock_ = true;
      clock_ts_ = ts;
      clock_time_ = p->time_us;
    }
    Insert(ts, p->spurt, p->main, s.arrival, false);
    if (playing_ && p->spurt == cur_spurt_) {
      NoteLevel(opus_packet_get_nb_samples(p->main.data(), static_cast<opus_int32>(p->main.size()), kSampleRate),
                s.arrival, now);
    }
    if (!p->red.empty()) {
      const int rs = opus_packet_get_nb_samples(p->red.data(), static_cast<opus_int32>(p->red.size()), kSampleRate);
      if (rs > 0) Insert(ts - rs, p->spurt, p->red, s.arrival, true);
    }
    if (p->end()) {
      const int ms = opus_packet_get_nb_samples(p->main.data(), static_cast<opus_int32>(p->main.size()), kSampleRate);
      end_spurt_ = p->spurt;
      end_ts_ = ts + std::max(ms, 0);
    }
  }
  ring_read_.store(r, std::memory_order_release);
}

void Receiver::UpdateTarget(uint64_t now) {
  if (now - last_target_update_ < 100'000 && last_target_update_ != 0) return;
  last_target_update_ = now;
  // Delays of the last 5 s (for the 99th percentile) and the minimum of the
  // last 10 s.
  size_t n = 0;
  int64_t lowest = std::numeric_limits<int64_t>::max();
  for (size_t i = 0; i < delays_count_; ++i) {
    const Delay& d = delays_[(delays_head_ + kDelays - 1 - i) % kDelays];
    const uint64_t age = now > d.time ? now - d.time : 0;
    if (age > kMinWindowUs) break;
    lowest = std::min(lowest, d.d);
    if (age <= kJitterWindowUs) scratch_[n++] = d.d;
  }
  double cap = kMaxTargetMs;
  if (last_underrun_ != 0 && now - last_underrun_ < kUnderrunHoldUs) cap = kUnderrunMaxTargetMs;
  if (n >= 20) {
    const size_t k = std::min(n - 1, n * 99 / 100);
    std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(k),
                     scratch_.begin() + static_cast<std::ptrdiff_t>(n));
    const double jitter_ms = static_cast<double>(scratch_[k] - lowest) / 1000.0;
    jitter_target_ms_ = std::clamp(jitter_ms + kJitterMarginMs, kMinTargetMs, cap);
  } else {
    jitter_target_ms_ = std::min(std::max(jitter_target_ms_, kStartTargetMs), cap);
  }
  // Sound against picture (D10): only while pictures come (a still screen
  // has no lips to match).
  const uint64_t vlast = video_last_us_.load();
  if (skew_valid_ && vlast != 0 && now - std::min(now, vlast) < 1'000'000) {
    if (skew_ms_ < -kAheadMs) {
      if (ahead_since_ == 0) ahead_since_ = now;
      // Once the buffer has followed the last change (the drift control fills
      // it slowly), delay the sound by what it is ahead.
      if (now - ahead_since_ >= kAheadForUs && std::abs(BufferMs() - target_ms_) < 5) {
        av_extra_ms_ = std::min(kMaxAvExtraMs, av_extra_ms_ - skew_ms_);
        ahead_since_ = 0;
      }
    } else {
      ahead_since_ = 0;
      // 10 ms a second (this runs every 100 ms) once it is no longer ahead.
      if (skew_ms_ >= 0) av_extra_ms_ = std::max(0.0, av_extra_ms_ - 1.0);
    }
  } else {
    ahead_since_ = 0;
  }
  target_ms_ = jitter_target_ms_ + av_extra_ms_;
  target_ms_out_ = static_cast<int32_t>(std::lround(target_ms_));
}

double Receiver::BufferMs() const {
  if (!playing_) return 0;
  const int64_t end = SpurtEnd(cur_spurt_);
  const double frames = static_cast<double>(std::max<int64_t>(0, end - play_ts_)) + resampler_.Buffered();
  return frames * 1000.0 / kSampleRate;
}

void Receiver::ResetDecoder() {
  opus_decoder_ctl(dec_, OPUS_RESET_STATE);
  dec_spurt_ = cur_spurt_;
  softclip_[0] = softclip_[1] = 0;
}

bool Receiver::TryStart(uint64_t now) {
  const Frame* first = nullptr;
  for (size_t i = 0; i < kFrames; ++i) {
    const Frame& f = frames_[i];
    if (!f.used) continue;
    // Behind what was played already: of no use.
    if (played_any_ && f.ts < play_ts_) continue;
    if (!first || f.ts < first->ts) first = &f;
  }
  if (!first) return false;
  const int spurt = first->spurt;
  const double have_ms = static_cast<double>(SpurtEnd(spurt) - first->ts) * 1000.0 / kSampleRate;
  const double waited_ms = static_cast<double>(now - std::min(now, first->arrival)) / 1000.0;
  const bool ended = end_spurt_ == spurt && end_ts_ <= SpurtEnd(spurt);
  if (have_ms < target_ms_ && waited_ms < target_ms_ && !ended) return false;
  const bool new_spurt = spurt != cur_spurt_ || dec_spurt_ != spurt;
  cur_spurt_ = spurt;
  play_ts_ = first->ts;
  if (new_spurt) ResetDecoder();
  resampler_.Reset();
  plc_run_ = 0;
  playing_ = true;
  played_any_ = true;
  flushing_ = false;
  level_count_ = 0;
  level_ms_ = target_ms_;
  over_since_ = 0;
  out_since_tick_ = 0;
  return true;
}

void Receiver::Stop(bool underrun, uint64_t now) {
  if (underrun) {
    ++underruns_;
    last_underrun_ = now;
  }
  playing_ = false;
  // What the resampler holds still plays (flushed with zeros behind it).
  static const float zeros[Resampler::kTaps * kChannels] = {};
  resampler_.Write(zeros, Resampler::kTaps);
  flushing_ = true;
  // Frames that can no longer be played go.
  for (size_t i = 0; i < kFrames; ++i) {
    Frame& f = frames_[i];
    if (f.used && f.ts < play_ts_) f.used = false;
  }
}

void Receiver::DecodeNext(uint64_t now) {
  Frame* f = Find(play_ts_, cur_spurt_);
  if (f) {
    const int n = opus_decode_float(dec_, f->data, f->len, pcm_.data(), kMaxFrameSamples, 0);
    f->used = false;
    if (n > 0) {
      opus_pcm_soft_clip(pcm_.data(), n, kChannels, softclip_);
      resampler_.Write(pcm_.data(), static_cast<size_t>(n));
      play_ts_ += n;
      last_samples_ = n;
      plc_run_ = 0;
      return;
    }
    // Undecodable: concealed below like a lost frame.
  }
  // Missing. Is the spurt over (END, a newer spurt, or 100 ms of nothing)?
  const int64_t end = SpurtEnd(cur_spurt_);
  const bool more = end > play_ts_;
  bool newer = false;
  for (size_t i = 0; i < kFrames && !newer; ++i) newer = frames_[i].used && frames_[i].spurt != cur_spurt_;
  const bool ended = (end_spurt_ == cur_spurt_ && play_ts_ >= end_ts_) ||
                     (!more && (newer || now - std::min(now, last_arrival_) > kIdleAfterUs));
  if (ended) {
    Stop(false, now);
    return;
  }
  if (plc_run_ >= kMaxConcealed) {
    // Ran dry: fade out, and wait for a target's worth again.
    const int n = opus_decode_float(dec_, nullptr, 0, pcm_.data(), std::min(last_samples_, kCrossfade), 0);
    if (n > 0) {
      for (int i = 0; i < n; ++i) {
        const float g = 1.0f - static_cast<float>(i + 1) / static_cast<float>(n);
        pcm_[2 * i] *= g;
        pcm_[2 * i + 1] *= g;
      }
      resampler_.Write(pcm_.data(), static_cast<size_t>(n));
    }
    Stop(true, now);
    return;
  }
  const int n = opus_decode_float(dec_, nullptr, 0, pcm_.data(), last_samples_, 0);
  ++plc_run_;
  ++lost_;
  if (n > 0) resampler_.Write(pcm_.data(), static_cast<size_t>(n));
  play_ts_ += last_samples_;
}

void Receiver::CatchUp() {
  // Skip what is over the target, with a crossfade from where the stream
  // was to where it goes on.
  const double excess_ms = BufferMs() - target_ms_;
  const int64_t skip = static_cast<int64_t>(excess_ms * kSampleRate / 1000.0);
  if (skip < last_samples_) return;
  // The old side: the next frame as it would have played.
  int old_n = 0;
  if (Frame* f = Find(play_ts_, cur_spurt_)) {
    old_n = opus_decode_float(dec_, f->data, f->len, pcm_.data(), kMaxFrameSamples, 0);
  }
  if (old_n > 0) {
    std::memcpy(fade_.data(), pcm_.data(), static_cast<size_t>(std::min(old_n, kCrossfade)) * kChannels * sizeof(float));
  }
  // Jump: whole frames from play_ts_ on, as far as the excess goes.
  int64_t target_ts = play_ts_ + skip;
  int64_t new_ts = play_ts_;
  for (;;) {
    Frame* f = Find(new_ts, cur_spurt_);
    if (!f || new_ts + f->samples > target_ts) break;
    f->used = false;
    new_ts += f->samples;
  }
  if (new_ts == play_ts_) return;
  play_ts_ = new_ts;
  ++catchups_;
  Frame* f = Find(play_ts_, cur_spurt_);
  if (!f) return;
  const int n = opus_decode_float(dec_, f->data, f->len, pcm_.data(), kMaxFrameSamples, 0);
  f->used = false;
  if (n <= 0) return;
  play_ts_ += n;
  last_samples_ = n;
  if (old_n > 0) {
    const int m = std::min({n, old_n, kCrossfade});
    for (int i = 0; i < m; ++i) {
      const float g = static_cast<float>(i + 1) / static_cast<float>(m + 1);
      for (int c = 0; c < kChannels; ++c) {
        pcm_[i * kChannels + c] = fade_[i * kChannels + c] * (1 - g) + pcm_[i * kChannels + c] * g;
      }
    }
  }
  opus_pcm_soft_clip(pcm_.data(), n, kChannels, softclip_);
  resampler_.Write(pcm_.data(), static_cast<size_t>(n));
}

void Receiver::Drift(uint64_t now) {
  // Every 100 ms of output: the buffer over the last second against the target.
  double sum = 0;
  size_t n = 0;
  for (size_t i = 0; i < level_count_; ++i) {
    const Level& l = levels_[(level_head_ + kLevels - 1 - i) % kLevels];
    if (now - std::min(now, l.time) > 1'000'000) break;
    sum += l.ms;
    ++n;
  }
  if (n == 0) return;  // no packets this last second: nothing to follow
  level_ms_ = sum / static_cast<double>(n);
  const double e = level_ms_ - target_ms_;
  integral_ = std::clamp(integral_ + 5 * e, -kMaxIntegral, kMaxIntegral);
  ppm_ = std::clamp(150 * e + integral_, -kMaxPpm, kMaxPpm);
  drift_ppm_ = static_cast<int32_t>(std::lround(ppm_));
}

size_t Receiver::Read(float* out, size_t frames, uint64_t play_time_us, uint64_t now_us) {
  if (play_time_us == 0) play_time_us = now_us;
  Drain(now_us);
  // How late the sound plays against the picture: the sample that reaches
  // the speaker at play_time_us is the one the resampler holds next; its
  // capture time follows from the last CLOCK. The clocks of the two machines
  // cancel out of the difference of the two delays.
  const int64_t vd = video_delay_us_.load();
  skew_valid_ = false;
  if (playing_ && have_clock_ && vd != INT64_MIN) {
    const double out_ts = static_cast<double>(play_ts_) - static_cast<double>(resampler_.Buffered());
    const double captured = static_cast<double>(clock_time_) + (out_ts - static_cast<double>(clock_ts_)) * 1e6 / kSampleRate;
    const double da = static_cast<double>(play_time_us) - captured;
    skew_ms_ = (da - static_cast<double>(vd)) / 1000.0;
    skew_valid_ = true;
    av_offset_ms_ = static_cast<int32_t>(std::lround(skew_ms_));
  }
  UpdateTarget(now_us);
  size_t done = 0, sound = 0;
  while (done < frames) {
    if (!playing_ && !flushing_ && !TryStart(now_us)) break;
    if (!playing_ && flushing_) {
      // The end of a spurt: what is left in the resampler, then silence (and
      // maybe the next spurt).
      const size_t got = resampler_.Read(out + done * kChannels, frames - done, 1.0);
      done += got;
      sound += got;
      if (done < frames) {
        flushing_ = false;
        resampler_.Reset();
      }
      continue;
    }
    // Too much buffered for too long: skip the excess.
    const double level = BufferMs();
    if (level > std::max(3 * target_ms_, target_ms_ + 100)) {
      if (over_since_ == 0) over_since_ = now_us;
      if (now_us - over_since_ >= kOverForUs) {
        CatchUp();
        over_since_ = 0;
        level_count_ = 0;
        level_ms_ = BufferMs();
      }
    } else {
      over_since_ = 0;
    }
    const double ratio = 1.0 + ppm_ / 1e6;
    const size_t want = frames - done;
    const size_t need = resampler_.Needed(want, ratio);
    int guard = 0;
    while (playing_ && resampler_.Buffered() < need && guard++ < 64) DecodeNext(now_us);
    const size_t got = resampler_.Read(out + done * kChannels, want, ratio);
    done += got;
    sound += got;
    out_since_tick_ += got;
    if (out_since_tick_ >= kTick && playing_) {
      out_since_tick_ -= kTick;
      Drift(now_us);
    }
    if (got == 0 && playing_) break;  // nothing more to be had (guard)
  }
  if (done < frames) std::memset(out + done * kChannels, 0, (frames - done) * kChannels * sizeof(float));
  buffer_ms_ = static_cast<int32_t>(std::lround(playing_ ? level_ms_ : 0));
  return sound;
}

}  // namespace rdacore::audio
