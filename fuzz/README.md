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
| `fuzz_recording` | `.rdstream` playback (what `rdaccel_play` and `rda_viewer_feed_recording()` read), through the platform's real HEVC decoder; seeded with real recordings |

Seed inputs live in `fuzz/corpus/<target>/` (for `recording`: synthetic
recordings at a few sizes and an Xvfb screen capture with the cursor, made
with `rdaccel_record`). Pass them as a second, read-only corpus directory.

Multi-channel targets read their input as `[channel][len][bytes]...`
(`fuzz_input.h`), so the fuzzer can interleave channels.

## Build and run (Linux, clang)

```sh
sudo apt-get install clang libclang-rt-18-dev   # libFuzzer runtime
F="-fsanitize=fuzzer-no-link,address,undefined"
CC=clang CXX=clang++ cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRDACCEL_BUILD_FUZZERS=ON -DRDACCEL_BUILD_TESTS=OFF -DRDACCEL_BUILD_EXAMPLES=OFF \
  -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F"
cmake --build build-fuzz -j
mkdir -p corpus/viewer && ./build-fuzz/fuzz/fuzz_viewer corpus/viewer -max_total_time=300
```

Reproduce a crash with `./build-fuzz/fuzz/fuzz_<name> crash-<sha>`.
Anything found should get a regression test under `tests/` with the fix.
