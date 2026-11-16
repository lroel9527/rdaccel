// System sound on macOS 14.2+: a Core Audio Process Tap of everything this
// computer plays (this process and any excluded applications left out), read
// through a private aggregate device that holds only the tap (no hardware:
// this side never opens an output device).
//
//   IOProc (real-time thread): copies the input and its host time into a
//   lock-free ring and signals; nothing else (no locks, allocation, logs).
//   Sound thread: to 48 kHz (AudioConverter, only when the tap runs at
//   another rate), to stereo (the first two channels; mono doubled), then
//   Source::PcmFn.
//   Rebuild thread: builds, tears down and rebuilds the tap, serially: on a
//   new default output device, a new tap format, coreaudiod restarting, the
//   device dying or stopping, a process to exclude appearing, the permission
//   changing; several within 300 ms count once; after a failure every 2 s.
//
// The permission (System Audio Recording) shows only when IO starts: making
// the tap asks nothing; AudioDeviceStart then asks the user once if they
// were never asked, and without the permission it succeeds all the same and
// delivers silence (measured on macOS 27.2). So, while the permission is not
// known to be there (TCC says not granted, or cannot say), the tap does not
// mute this computer (CATapUnmuted: muting while hearing nothing would leave
// both sides silent), and it is rebuilt every 5 s so that a permission given
// meanwhile takes effect. A non-zero sample proves the permission; so does
// TCC saying granted. Only then does a tap that is to mute this computer
// (SourceConfig::mute_local, CATapMutedWhenTapped) replace it.
//
// The tap and the aggregate device belong to this process: when it dies,
// coreaudiod removes them and this computer's sound is back. Nothing
// persistent (volume, mute, the default device) is ever changed.

#include "mac/audio_tap.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_source.h"
#include "capture/capturer.h"
#include "log/log.h"

namespace rdacore::platform {

int AudioCapturePermission() {
  using PreflightFn = int (*)(CFStringRef, CFDictionaryRef);
  static PreflightFn preflight = [] {
    void* h = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC", RTLD_LAZY);
    return h ? reinterpret_cast<PreflightFn>(dlsym(h, "TCCAccessPreflight")) : nullptr;
  }();
  if (!preflight) return -1;
  const int r = preflight(CFSTR("kTCCServiceAudioCapture"), nullptr);
  return r >= 0 && r <= 2 ? r : -1;
}

uint64_t HostTimeToMicros(uint64_t host_time) {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    return t;
  }();
  auto us = [&](uint64_t ticks) {
    return static_cast<uint64_t>(static_cast<__uint128_t>(ticks) * tb.numer / tb.denom / 1000);
  };
  // steady_clock (capture::MonotonicMicros) and mach_absolute_time are the
  // same clock while a Mac has not slept; the difference covers one that
  // has (steady_clock may count the sleep).
  const int64_t offset = static_cast<int64_t>(capture::MonotonicMicros()) - static_cast<int64_t>(us(mach_absolute_time()));
  return static_cast<uint64_t>(static_cast<int64_t>(us(host_time)) + offset);
}

}  // namespace rdacore::platform

namespace rdacore::audio {
namespace {

constexpr uint64_t kRetryUs = 2'000'000;
constexpr uint64_t kCoalesceUs = 300'000;
constexpr uint64_t kUnconfirmedRebuildUs = 5'000'000;
constexpr uint64_t kPermissionCheckUs = 2'000'000;
constexpr uint64_t kDeniedCheckUs = 5'000'000;

uint64_t Now() { return capture::MonotonicMicros(); }

std::string OsStatusText(OSStatus st) {
  const uint32_t be = CFSwapInt32HostToBig(static_cast<uint32_t>(st));
  char c[5] = {};
  std::memcpy(c, &be, 4);
  bool printable = true;
  for (int i = 0; i < 4; ++i) printable &= c[i] >= 32 && c[i] < 127;
  return printable ? "'" + std::string(c) + "'" : std::to_string(st);
}

AudioObjectPropertyAddress Addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
  return {sel, scope, kAudioObjectPropertyElementMain};
}

// Single writer (the IOProc), single reader (the sound thread): raw tap
// frames and, per IOProc call, how many and when.
class Ring {
 public:
  void Reset(size_t floats, size_t chunks) {
    data_.assign(floats, 0.0f);
    chunks_.assign(chunks, {});
    w_ = r_ = cw_ = cr_ = 0;
  }
  struct Chunk {
    uint64_t host_time = 0;
    uint32_t frames = 0;
  };
  // Real-time thread. False (and nothing written) when full.
  bool Write(const float* p, size_t floats, uint32_t frames, uint64_t host_time) {
    const size_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
    const size_t cw = cw_.load(std::memory_order_relaxed), cr = cr_.load(std::memory_order_acquire);
    if (data_.size() - (w - r) < floats || cw - cr >= chunks_.size()) return false;
    for (size_t i = 0; i < floats; ++i) data_[(w + i) % data_.size()] = p ? p[i] : 0.0f;
    chunks_[cw % chunks_.size()] = {host_time, frames};
    w_.store(w + floats, std::memory_order_release);
    cw_.store(cw + 1, std::memory_order_release);
    return true;
  }
  bool Read(Chunk& c, std::vector<float>& out, size_t floats_per_frame) {
    const size_t cw = cw_.load(std::memory_order_acquire), cr = cr_.load(std::memory_order_relaxed);
    if (cw == cr) return false;
    c = chunks_[cr % chunks_.size()];
    const size_t n = c.frames * floats_per_frame;
    out.resize(n);
    const size_t r = r_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) out[i] = data_[(r + i) % data_.size()];
    r_.store(r + n, std::memory_order_release);
    cr_.store(cr + 1, std::memory_order_release);
    return true;
  }

 private:
  std::vector<float> data_;
  std::vector<Chunk> chunks_;
  std::atomic<size_t> w_{0}, r_{0}, cw_{0}, cr_{0};
};

class TapSource final : public Source {
 public:
  TapSource() { semaphore_create(mach_task_self(), &sem_, SYNC_POLICY_FIFO, 0); }
  ~TapSource() override {
    Stop();
    semaphore_destroy(mach_task_self(), sem_);
  }

  bool muting_local() const override { return muting_.load(); }

  bool Start(const SourceConfig& config, PcmFn pcm, StateFn state) override {
    Stop();
    config_ = config;
    pcm_ = std::move(pcm);
    state_fn_ = std::move(state);
    stop_ = false;
    rebuild_thread_ = std::thread([this] { RebuildLoop(); });
    return true;
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> l(mu_);
      if (!rebuild_thread_.joinable()) return;
      stop_ = true;
    }
    cv_.notify_all();
    rebuild_thread_.join();  // tears the tap down on its way out
  }

 private:
  // ---- rebuild thread -------------------------------------------------------

  void SetState(SourceState s, const std::string& msg) {
    if (s == last_state_ && msg == last_message_) return;
    last_state_ = s;
    last_message_ = msg;
    if (state_fn_) state_fn_(s, msg);
  }

  void Due(bool processes = false) {
    {
      std::lock_guard<std::mutex> l(mu_);
      if (processes) {
        procs_due_ = true;
      } else {
        everything_due_ = true;
      }
      if (rebuild_due_ == 0) rebuild_due_ = Now() + kCoalesceUs;
    }
    cv_.notify_all();
  }

  void RebuildLoop() {
    @autoreleasepool {
      AddSystemListeners();
      uint64_t next_try = 0, built_at = 0, last_check = 0;
      bool built = false;
      SetState(SourceState::kStarting, "");
      for (;;) @autoreleasepool {
        {
          std::unique_lock<std::mutex> l(mu_);
          const uint64_t now = Now();
          uint64_t wake = now + 250'000;
          if (!built && next_try > now) wake = std::min(wake, next_try);
          if (rebuild_due_ != 0) wake = std::min(wake, rebuild_due_);
          cv_.wait_for(l, std::chrono::microseconds(wake > now ? wake - now : 0), [&] {
            return stop_ || (rebuild_due_ != 0 && Now() >= rebuild_due_);
          });
          if (stop_) break;
        }
        const uint64_t now = Now();
        bool rebuild = false, procs = false;
        {
          std::lock_guard<std::mutex> l(mu_);
          if (rebuild_due_ != 0 && now >= rebuild_due_) {
            rebuild = everything_due_;
            procs = procs_due_;
            rebuild_due_ = 0;
            everything_due_ = procs_due_ = false;
          }
        }
        // A process came or went: only if that changes what is left out (a
        // rebuild is a moment of this computer's own sound when it mutes, so
        // not for every application that starts playing).
        if (procs && built && ![Excluded() isEqualToArray:excluded_]) rebuild = true;
        // The IO stopped without us (the device went away, coreaudiod
        // restarted): rebuild.
        if (built && io_dead_.exchange(false)) rebuild = true;
        // The permission: confirmed by sound or by TCC.
        const bool confirmed = confirmed_.load();
        if (!confirmed && now - last_check >= kPermissionCheckUs) {
          last_check = now;
          const int perm = platform::AudioCapturePermission();
          if (perm == 0) {
            confirmed_ = true;
          } else if (perm == 1) {
            if (built) Teardown();
            built = false;
            SetState(SourceState::kNoPermission,
                     "this computer's System Audio Recording permission is denied (System Settings, Privacy & "
                     "Security, Screen & System Audio Recording)");
            next_try = now + kDeniedCheckUs;
            continue;
          }
        }
        if (built) {
          // Mute only once the permission is known (and the tap does not yet).
          if (confirmed_ && config_.mute_local && !muting_wanted_built_) rebuild = true;
          // Not known yet: rebuild now and then, so a permission given
          // meanwhile reaches the capture (it is checked when IO starts).
          if (!confirmed_ && now - built_at >= kUnconfirmedRebuildUs) rebuild = true;
          if (!rebuild) continue;
          Teardown();
          built = false;
          next_try = 0;
        }
        if (!built && now >= next_try) {
          std::string err;
          if (Build(&err)) {
            built = true;
            built_at = Now();
            SetState(SourceState::kRunning, "");
          } else {
            Teardown();
            SetState(SourceState::kFailed, err);
            next_try = Now() + kRetryUs;
          }
        }
      }
      Teardown();
      RemoveSystemListeners();
    }
  }

  // Process objects to leave out: this process, and running applications
  // whose bundle identifier is in the config.
  NSArray<NSNumber*>* Excluded() {
    NSMutableArray<NSNumber*>* out = [NSMutableArray array];
    pid_t pid = getpid();
    AudioObjectID self_obj = kAudioObjectUnknown;
    UInt32 size = sizeof(self_obj);
    auto a = Addr(kAudioHardwarePropertyTranslatePIDToProcessObject);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(pid), &pid, &size, &self_obj) == noErr &&
        self_obj != kAudioObjectUnknown) {
      [out addObject:@(self_obj)];
    }
    if (config_.exclude_bundle_ids.empty()) return out;
    a = Addr(kAudioHardwarePropertyProcessObjectList);
    size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return out;
    std::vector<AudioObjectID> procs(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, procs.data()) != noErr) return out;
    procs.resize(size / sizeof(AudioObjectID));
    for (AudioObjectID p : procs) {
      CFStringRef bid = nullptr;
      UInt32 s = sizeof(bid);
      auto b = Addr(kAudioProcessPropertyBundleID);
      if (AudioObjectGetPropertyData(p, &b, 0, nullptr, &s, &bid) != noErr || !bid) continue;
      NSString* id = CFBridgingRelease(bid);
      for (const auto& want : config_.exclude_bundle_ids) {
        if ([id isEqualToString:@(want.c_str())]) [out addObject:@(p)];
      }
    }
    return out;
  }

  bool Build(std::string* error) {
    muting_wanted_built_ = confirmed_ && config_.mute_local;
    io_dead_ = false;
    excluded_ = Excluded();
    CATapDescription* d = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded_];
    d.name = @"rdaccel";
    d.UUID = [NSUUID UUID];
    d.privateTap = YES;
    // Muted only with the permission known (see the top of the file).
    d.muteBehavior = muting_wanted_built_ ? CATapMutedWhenTapped : CATapUnmuted;
    OSStatus st = AudioHardwareCreateProcessTap(d, &tap_);
    if (st != noErr || tap_ == kAudioObjectUnknown) {
      tap_ = kAudioObjectUnknown;
      *error = "cannot create the process tap (" + OsStatusText(st) + ")";
      return false;
    }
    // Its format: 32-bit float, 1 to 8 channels, 8 to 192 kHz.
    AudioStreamBasicDescription f{};
    UInt32 size = sizeof(f);
    auto a = Addr(kAudioTapPropertyFormat);
    st = AudioObjectGetPropertyData(tap_, &a, 0, nullptr, &size, &f);
    if (st != noErr || f.mFormatID != kAudioFormatLinearPCM || !(f.mFormatFlags & kAudioFormatFlagIsFloat) ||
        f.mBitsPerChannel != 32 || f.mChannelsPerFrame < 1 || f.mChannelsPerFrame > 8 || f.mSampleRate < 8000 ||
        f.mSampleRate > 192000) {
      *error = "unexpected tap format (" + std::to_string(f.mChannelsPerFrame) + " channels, " +
               std::to_string(static_cast<int>(f.mSampleRate)) + " Hz, " + std::to_string(f.mBitsPerChannel) +
               " bits, flags " + std::to_string(f.mFormatFlags) + ")";
      return false;
    }
    format_ = f;
    channels_ = static_cast<int>(f.mChannelsPerFrame);
    ring_.Reset(static_cast<size_t>(f.mSampleRate) * static_cast<size_t>(channels_), 512);
    // A private aggregate device of the tap alone: no hardware sub-device,
    // so no output device is ever opened here. Its IOProc runs on the tap's
    // own clock.
    NSString* uid = [@"org.rdaccel.tap." stringByAppendingString:d.UUID.UUIDString];
    NSDictionary* desc = @{
      @kAudioAggregateDeviceNameKey : @"rdaccel audio tap",
      @kAudioAggregateDeviceUIDKey : uid,
      @kAudioAggregateDeviceIsPrivateKey : @1,
      @kAudioAggregateDeviceIsStackedKey : @0,
      @kAudioAggregateDeviceTapAutoStartKey : @0,
      @kAudioAggregateDeviceTapListKey : @[ @{
        @kAudioSubTapUIDKey : d.UUID.UUIDString,
        @kAudioSubTapDriftCompensationKey : @1,
      } ],
    };
    st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)desc, &device_);
    if (st != noErr || device_ == kAudioObjectUnknown) {
      device_ = kAudioObjectUnknown;
      *error = "cannot create the aggregate device for the tap (" + OsStatusText(st) + ")";
      return false;
    }
    AddDeviceListeners();
    st = AudioDeviceCreateIOProcID(device_, &IoProc, this, &proc_);
    if (st != noErr) {
      proc_ = nullptr;
      *error = "cannot add an IOProc (" + OsStatusText(st) + ")";
      return false;
    }
    // The sound thread before the IO.
    sound_stop_ = false;
    sound_thread_ = std::thread([this] { SoundLoop(); });
    st = AudioDeviceStart(device_, proc_);
    if (st != noErr) {
      *error = "cannot start the tap's IO (" + OsStatusText(st) + ")";
      return false;
    }
    started_ = true;
    muting_ = muting_wanted_built_;
    log::Info(std::string("sound: process tap running, ") + std::to_string(channels_) + " channels at " +
              std::to_string(static_cast<int>(format_.mSampleRate)) + " Hz" +
              (muting_wanted_built_ ? ", this computer muted while it is read" : ""));
    return true;
  }

  void Teardown() {
    // In this order: IO, IOProc, device, tap (our own stop is no news to
    // the listeners).
    muting_ = false;
    if (device_ != kAudioObjectUnknown) RemoveDeviceListeners();
    if (device_ != kAudioObjectUnknown && proc_ && started_) AudioDeviceStop(device_, proc_);
    started_ = false;
    if (device_ != kAudioObjectUnknown && proc_) AudioDeviceDestroyIOProcID(device_, proc_);
    proc_ = nullptr;
    if (sound_thread_.joinable()) {
      sound_stop_ = true;
      semaphore_signal(sem_);
      sound_thread_.join();
    }
    if (device_ != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(device_);
    device_ = kAudioObjectUnknown;
    if (tap_ != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(tap_);
    tap_ = kAudioObjectUnknown;
    if (converter_) {
      AudioConverterDispose(converter_);
      converter_ = nullptr;
    }
  }

  // ---- listeners ------------------------------------------------------------

  static OSStatus OnSystemChange(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* addrs, void* self) {
    auto* t = static_cast<TapSource*>(self);
    for (UInt32 i = 0; i < n; ++i) {
      if (addrs[i].mSelector == kAudioHardwarePropertyProcessObjectList) {
        // Only an application to exclude can change anything.
        if (!t->config_.exclude_bundle_ids.empty()) t->Due(/*processes=*/true);
        continue;
      }
      t->Due();
    }
    return noErr;
  }

  static OSStatus OnDeviceChange(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress* addrs, void* self) {
    auto* t = static_cast<TapSource*>(self);
    for (UInt32 i = 0; i < n; ++i) {
      if (addrs[i].mSelector == kAudioDevicePropertyDeviceIsAlive ||
          addrs[i].mSelector == kAudioDevicePropertyDeviceIsRunning) {
        UInt32 v = 1, size = sizeof(v);
        AudioObjectPropertyAddress a = addrs[i];
        if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &v) == noErr && v == 0) {
          t->io_dead_ = true;
          t->Due();
        }
      } else {
        t->Due();  // the tap's format
      }
    }
    return noErr;
  }

  static constexpr AudioObjectPropertySelector kSystemSelectors[] = {
      kAudioHardwarePropertyDefaultOutputDevice, kAudioHardwarePropertyServiceRestarted,
      kAudioHardwarePropertyProcessObjectList};

  void AddSystemListeners() {
    for (auto sel : kSystemSelectors) {
      auto a = Addr(sel);
      AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, &OnSystemChange, this);
    }
  }
  void RemoveSystemListeners() {
    for (auto sel : kSystemSelectors) {
      auto a = Addr(sel);
      AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &a, &OnSystemChange, this);
    }
  }
  void AddDeviceListeners() {
    for (auto sel : {kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyDeviceIsRunning}) {
      auto a = Addr(sel);
      AudioObjectAddPropertyListener(device_, &a, &OnDeviceChange, this);
    }
    auto f = Addr(kAudioTapPropertyFormat);
    AudioObjectAddPropertyListener(tap_, &f, &OnDeviceChange, this);
  }
  void RemoveDeviceListeners() {
    for (auto sel : {kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyDeviceIsRunning}) {
      auto a = Addr(sel);
      AudioObjectRemovePropertyListener(device_, &a, &OnDeviceChange, this);
    }
    if (tap_ != kAudioObjectUnknown) {
      auto f = Addr(kAudioTapPropertyFormat);
      AudioObjectRemovePropertyListener(tap_, &f, &OnDeviceChange, this);
    }
  }

  // ---- real-time thread -----------------------------------------------------

  static OSStatus IoProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp* in_time,
                         AudioBufferList*, const AudioTimeStamp*, void* self) {
    auto* t = static_cast<TapSource*>(self);
    if (!in || in->mNumberBuffers == 0 || !in_time) return noErr;
    const uint64_t host = (in_time->mFlags & kAudioTimeStampHostTimeValid) ? in_time->mHostTime : 0;
    if (in->mNumberBuffers == 1) {
      // Interleaved (what taps deliver).
      const AudioBuffer& b = in->mBuffers[0];
      const UInt32 ch = std::max<UInt32>(1, b.mNumberChannels);
      const uint32_t frames = b.mDataByteSize / (sizeof(float) * ch);
      if (ch == static_cast<UInt32>(t->channels_)) {
        t->ring_.Write(static_cast<const float*>(b.mData), frames * ch, frames, host);
      }
    } else {
      // One buffer per channel: interleave the first two into a scratch the
      // size of one call (preallocated; a larger call is dropped).
      const uint32_t frames = in->mBuffers[0].mDataByteSize / sizeof(float);
      const int ch = std::min<int>(t->channels_, static_cast<int>(in->mNumberBuffers));
      if (frames * static_cast<uint32_t>(t->channels_) <= kScratch) {
        for (uint32_t i = 0; i < frames; ++i) {
          for (int c = 0; c < t->channels_; ++c) {
            const auto* src = static_cast<const float*>(in->mBuffers[std::min(c, ch - 1)].mData);
            t->scratch_[i * t->channels_ + c] = src ? src[i] : 0.0f;
          }
        }
        t->ring_.Write(t->scratch_, frames * t->channels_, frames, host);
      }
    }
    semaphore_signal(t->sem_);
    return noErr;
  }

  // ---- sound thread ---------------------------------------------------------

  struct ConvInput {
    const float* data;
    UInt32 frames;
  };
  static OSStatus ConverterInput(AudioConverterRef, UInt32* packets, AudioBufferList* data, AudioStreamPacketDescription**,
                                 void* user) {
    auto* in = static_cast<ConvInput*>(user);
    if (in->frames == 0) {
      *packets = 0;
      return 1;  // no more for now
    }
    *packets = std::min(*packets, in->frames);
    data->mNumberBuffers = 1;
    data->mBuffers[0].mNumberChannels = 2;
    data->mBuffers[0].mDataByteSize = *packets * 2 * sizeof(float);
    data->mBuffers[0].mData = const_cast<float*>(in->data);
    in->data += *packets * 2;
    in->frames -= *packets;
    return noErr;
  }

  void SoundLoop() {
    std::vector<float> raw, stereo, out;
    const double rate = format_.mSampleRate;
    if (std::abs(rate - kSampleRate) > 0.5) {
      AudioStreamBasicDescription a{};
      a.mSampleRate = rate;
      a.mFormatID = kAudioFormatLinearPCM;
      a.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
      a.mBytesPerPacket = a.mBytesPerFrame = 2 * sizeof(float);
      a.mFramesPerPacket = 1;
      a.mChannelsPerFrame = 2;
      a.mBitsPerChannel = 32;
      AudioStreamBasicDescription b = a;
      b.mSampleRate = kSampleRate;
      if (AudioConverterNew(&a, &b, &converter_) == noErr) {
        UInt32 q = kAudioConverterQuality_High;
        AudioConverterSetProperty(converter_, kAudioConverterSampleRateConverterQuality, sizeof(q), &q);
      } else {
        converter_ = nullptr;
      }
    }
    uint64_t next_us = 0;  // the next converted sample's time
    while (!sound_stop_) {
      semaphore_timedwait(sem_, {0, 100'000'000});
      Ring::Chunk c;
      while (ring_.Read(c, raw, static_cast<size_t>(channels_))) {
        // To stereo: the first two channels, a single one doubled.
        stereo.resize(static_cast<size_t>(c.frames) * 2);
        bool sound = false;
        for (uint32_t i = 0; i < c.frames; ++i) {
          const float l = raw[i * channels_];
          const float r = channels_ > 1 ? raw[i * channels_ + 1] : l;
          stereo[2 * i] = l;
          stereo[2 * i + 1] = r;
          sound |= l != 0.0f || r != 0.0f;
        }
        // Any sample at all proves the permission (without it the tap gives zeros).
        if (sound && !confirmed_.exchange(true)) log::Info("sound: the capture hears this computer");
        const uint64_t t = c.host_time ? platform::HostTimeToMicros(c.host_time) : Now();
        if (!converter_) {
          pcm_(stereo.data(), c.frames, t);
          continue;
        }
        // Another rate: through the converter, timed by the chunk it came from.
        ConvInput in{stereo.data(), c.frames};
        const auto max_out = static_cast<UInt32>(std::ceil(c.frames * kSampleRate / rate)) + 64;
        out.resize(static_cast<size_t>(max_out) * 2);
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 2;
        list.mBuffers[0].mDataByteSize = max_out * 2 * sizeof(float);
        list.mBuffers[0].mData = out.data();
        UInt32 frames_out = max_out;
        AudioConverterFillComplexBuffer(converter_, &ConverterInput, &in, &frames_out, &list, nullptr);
        if (frames_out == 0) continue;
        if (next_us == 0 || t > next_us + 50'000 || t + 50'000 < next_us) next_us = t;
        pcm_(out.data(), frames_out, next_us);
        next_us += static_cast<uint64_t>(frames_out) * 1'000'000 / kSampleRate;
      }
    }
  }

  static constexpr size_t kScratch = 8 * 4096;

  SourceConfig config_;
  PcmFn pcm_;
  StateFn state_fn_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  uint64_t rebuild_due_ = 0;
  bool everything_due_ = false, procs_due_ = false;
  NSArray<NSNumber*>* excluded_ = nil;  // what the tap leaves out
  std::thread rebuild_thread_;
  SourceState last_state_ = SourceState::kFailed;
  std::string last_message_ = "-";

  // Built by the rebuild thread.
  AudioObjectID tap_ = kAudioObjectUnknown;
  AudioObjectID device_ = kAudioObjectUnknown;
  AudioDeviceIOProcID proc_ = nullptr;
  bool started_ = false;
  bool muting_wanted_built_ = false;
  AudioStreamBasicDescription format_{};
  int channels_ = 2;
  Ring ring_;
  float scratch_[kScratch] = {};
  semaphore_t sem_{};
  std::thread sound_thread_;
  std::atomic<bool> sound_stop_{false};
  AudioConverterRef converter_ = nullptr;
  std::atomic<bool> confirmed_{false};  // the permission is known to be there
  std::atomic<bool> muting_{false};
  std::atomic<bool> io_dead_{false};
};

}  // namespace

std::unique_ptr<Source> CreateSystemSource() { return std::make_unique<TapSource>(); }

}  // namespace rdacore::audio
