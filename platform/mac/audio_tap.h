#pragma once

// macOS 14.2: everything this computer plays, through a Core Audio Process
// Tap (audio::CreateSystemSource()). See audio_tap.mm.

#include <cstdint>

namespace rdacore::platform {

// The System Audio Recording permission (TCC kTCCServiceAudioCapture), from
// TCC's private TCCAccessPreflight (looked up at run time; there is no
// public call): 0 granted, 1 denied, 2 not asked yet, -1 not known (the
// function is missing). TCC answers each process from a cache, so a choice
// made while the process runs may show only in the next one; a capture that
// delivers sound proves the permission all the same.
int AudioCapturePermission();

// Whether a tap may mute this computer: never while RDACCEL_AUDIO_NULL_OUTPUT
// or RDACCEL_AUDIO_NO_LOCAL_MUTE is set (CTest sets the first for every test):
// a test can then never silence the machine it runs on.
bool LocalMuteAllowed();
// The same from the two variables' values (null: not set), for tests that
// must not unset the variable that keeps them safe.
bool LocalMuteAllowedFor(const char* null_output, const char* no_local_mute);
// What the tap is built as: muting (CATapMutedWhenTapped) only when asked
// for, with the permission known to be there, and allowed (above).
bool TapMutesLocal(bool permission_confirmed, bool mute_local);
bool TapMutesLocalWith(bool permission_confirmed, bool mute_local, bool allowed);

// mach_absolute_time() ticks in microseconds: the clock of
// capture::MonotonicMicros() (steady_clock is mach_absolute_time on macOS).
uint64_t HostTimeToMicros(uint64_t host_time);

}  // namespace rdacore::platform
