#pragma once

// Recordings (.rdstream): the exact bytes a Host sends for the screen, behind
// an 8-byte magic. A recording made on one platform plays back on another,
// which makes them useful for bitrate comparisons and interop tests.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rdaccel/transport.h"

namespace rdaccel {

// A transport for Host that writes the stream to `path` instead of sending it.
std::unique_ptr<Transport> CreateRecordingTransport(const std::string& path,
                                                    std::string* error = nullptr);

// Reads a whole recording, for Viewer::FeedRecording().
bool ReadRecording(const std::string& path, std::vector<uint8_t>& out,
                   std::string* error = nullptr);

}  // namespace rdaccel
