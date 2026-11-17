# Fuzzing

libFuzzer targets for every place rdaccel parses bytes from the network or
from a recording. Before a release, run each for about 60 s with ASan and
UBSan (see below); a crash leaves its reproducer in the working directory.

| target | what it feeds |
|---|---|
| `fuzz_messages` | every wire message, through the stream parser with arbitrary splits; parsed messages must re-encode and parse back identically |
| `fuzz_lossless` | the lossless layer decoder (deflate stream, region ops, palette tiles) |
| `fuzz_viewer` | the viewer session end to end on every channel, with a fake video decoder |
| `fuzz_link` | the connection layer (handshake, keepalive, goodbye, app messages), host and viewer role |
| `fuzz_secure` | the encrypted transport's receive side: peer hello, record framing, alerts, bad tags |
| `fuzz_reliable` | the UDP reliability layer's datagram parser |
| `fuzz_hevc` | the HEVC Annex B / access unit helpers |
| `fuzz_audio` | the sound receiver (1.13): any datagram as a sound packet, then reads as a player makes them, through the Opus decoder, the jitter buffer and the resampler |
| `fuzz_recording` | `.rdstream` playback (what `rdaccel_play` and `rda_viewer_feed_recording()` read), through the platform's real HEVC decoder; seeded with real recordings |

Seed inputs live in `fuzz/corpus/<target>/`; the repository carries them
for `reliable` only. For `recording`, make some with `rdaccel_record`
(synthetic recordings at a few sizes, and a screen capture with the cursor).
Pass them as a second, read-only corpus directory.

Multi-channel targets read their input as `[channel][len][bytes]...`
(`fuzz_input.h`), so the fuzzer can interleave channels.

## Build and run (macOS, LLVM clang)

rdaccel builds on macOS only (2.0). The clang that comes with Xcode and the
Command Line Tools has no libFuzzer runtime (`-fsanitize=fuzzer` fails at
link time), so the fuzzers need an LLVM clang that ships it, such as
Homebrew's. The `.mm` sources are compiled as Objective-C++, which has its
own compiler and flags variables in CMake.

These commands are the 1.x Linux ones carried over to macOS. They were not
run when this file was updated for 2.0, so expect to adjust them.

```sh
brew install llvm                               # clang with the libFuzzer runtime
LLVM="$(brew --prefix llvm)/bin"
F="-fsanitize=fuzzer-no-link,address,undefined"
CC="$LLVM/clang" CXX="$LLVM/clang++" OBJCXX="$LLVM/clang++" \
  cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRDACCEL_BUILD_FUZZERS=ON -DRDACCEL_BUILD_TESTS=OFF -DRDACCEL_BUILD_EXAMPLES=OFF \
  -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" -DCMAKE_OBJCXX_FLAGS="$F"
cmake --build build-fuzz -j
mkdir -p corpus/viewer && ./build-fuzz/fuzz/fuzz_viewer corpus/viewer -max_total_time=300
```

Reproduce a crash with `./build-fuzz/fuzz/fuzz_<name> crash-<sha>`.
Anything found should get a regression test under `tests/` with the fix.
