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

// mach_absolute_time() ticks in microseconds: the clock of
// capture::MonotonicMicros() (steady_clock is mach_absolute_time on macOS).
uint64_t HostTimeToMicros(uint64_t host_time);

}  // namespace rdacore::platform
