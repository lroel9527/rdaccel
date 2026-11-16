// The viewer's sound on the default output device: an output AudioUnit of
// subtype DefaultOutput, which follows the default device when the user
// changes it. 48 kHz stereo float in; the unit converts to the device's
// rate. A 256-frame IO buffer is asked for (this process's request, not a
// setting of the device). Volume and mute are the system's: this file only
// ever reads them (OutputMuted), never sets anything.
//
// Never opened when RDACCEL_AUDIO_NULL_OUTPUT=1 (audio/player.h): the C
// API then makes a null player instead, and so does every test.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <string>

#include "audio/player.h"
#include "mac/audio_tap.h"

namespace rdacore::audio {
namespace {

std::atomic<uint64_t> g_opened{0};

AudioObjectPropertyAddress Addr(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope,
                                AudioObjectPropertyElement el = kAudioObjectPropertyElementMain) {
  return {sel, scope, el};
}

AudioDeviceID DefaultOutput() {
  AudioDeviceID dev = kAudioObjectUnknown;
  UInt32 size = sizeof(dev);
  auto a = Addr(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &dev) != noErr) {
    return kAudioObjectUnknown;
  }
  return dev;
}

// Read-only: is the default output muted or at volume 0?
int QueryOutputMuted() {
  const AudioDeviceID dev = DefaultOutput();
  if (dev == kAudioObjectUnknown) return -1;
  bool known = false;
  UInt32 mute = 0, size = sizeof(mute);
  auto m = Addr(kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput);
  if (AudioObjectHasProperty(dev, &m) && AudioObjectGetPropertyData(dev, &m, 0, nullptr, &size, &mute) == noErr) {
    if (mute) return 1;
    known = true;
  }
  Float32 vol = 1;
  size = sizeof(vol);
  // The volume the menu bar shows; else the device's own main control.
  auto v = Addr(kAudioHardwareServiceDeviceProperty_VirtualMainVolume, kAudioDevicePropertyScopeOutput);
  if (!AudioObjectHasProperty(dev, &v)) v = Addr(kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput);
  if (AudioObjectHasProperty(dev, &v) && AudioObjectGetPropertyData(dev, &v, 0, nullptr, &size, &vol) == noErr) {
    if (vol <= 0.0f) return 1;
    known = true;
  }
  return known ? 0 : -1;
}

class DevicePlayer final : public Player {
 public:
  explicit DevicePlayer(RenderFn render) : render_(std::move(render)) {}
  ~DevicePlayer() override {
    if (unit_) {
      AudioOutputUnitStop(unit_);
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
    }
  }

  bool Open(std::string* error) {
    AudioComponentDescription d{};
    d.componentType = kAudioUnitType_Output;
    d.componentSubType = kAudioUnitSubType_DefaultOutput;
    d.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent c = AudioComponentFindNext(nullptr, &d);
    if (!c || AudioComponentInstanceNew(c, &unit_) != noErr) {
      unit_ = nullptr;
      if (error) *error = "no default output unit";
      return false;
    }
    ++g_opened;
    AudioStreamBasicDescription f{};
    f.mSampleRate = 48000;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = 2 * sizeof(float);
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    OSStatus st = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f));
    UInt32 frames = 256;
    AudioUnitSetProperty(unit_, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames, sizeof(frames));
    AURenderCallbackStruct cb{&Render, this};
    if (st == noErr) {
      st = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    }
    if (st == noErr) st = AudioUnitInitialize(unit_);
    if (st == noErr) st = AudioOutputUnitStart(unit_);
    if (st != noErr) {
      if (error) *error = "cannot start the output unit (" + std::to_string(st) + ")";
      return false;
    }
    return true;
  }

  int OutputMuted() const override { return QueryOutputMuted(); }
  uint64_t frames() const override { return frames_; }

 private:
  // The audio unit's render thread (real time): straight through to the
  // viewer, which neither blocks nor allocates.
  static OSStatus Render(void* self, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32, UInt32 frames,
                         AudioBufferList* data) {
    auto* p = static_cast<DevicePlayer*>(self);
    if (!data || data->mNumberBuffers < 1) return noErr;
    auto* out = static_cast<float*>(data->mBuffers[0].mData);
    // When this buffer reaches the speaker: its host time (the device's
    // latency comes on top; a few ms, the same for every buffer).
    const uint64_t play = ts && (ts->mFlags & kAudioTimeStampHostTimeValid) ? platform::HostTimeToMicros(ts->mHostTime) : 0;
    p->render_(out, frames, play);
    p->frames_ += frames;
    return noErr;
  }

  RenderFn render_;
  AudioComponentInstance unit_ = nullptr;
  std::atomic<uint64_t> frames_{0};
};

}  // namespace

std::unique_ptr<Player> CreateDevicePlayer(RenderFn render, std::string* error) {
  auto p = std::make_unique<DevicePlayer>(std::move(render));
  if (!p->Open(error)) return nullptr;
  return p;
}

uint64_t DevicePlayersOpened() { return g_opened; }

}  // namespace rdacore::audio
