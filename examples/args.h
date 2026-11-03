#pragma once

// Tiny --key=value command line parser shared by the examples.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace rdaccel::examples {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) != 0) {
        positional_ = a;
        continue;
      }
      a = a.substr(2);
      const auto eq = a.find('=');
      if (eq == std::string::npos) {
        kv_[a] = "1";
      } else {
        kv_[a.substr(0, eq)] = a.substr(eq + 1);
      }
    }
  }

  // The last argument that does not start with "--".
  const std::string& positional() const { return positional_; }

  bool has(const std::string& k) const { return kv_.count(k) > 0; }
  std::string str(const std::string& k, const std::string& def = "") const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  long long num(const std::string& k, long long def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::atoll(it->second.c_str());
  }
  double real(const std::string& k, double def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::atof(it->second.c_str());
  }
  // --key=<64 hex digits>: a 32-byte encryption key. Returns false (and
  // prints why) if it is present but malformed; `out` stays empty without it.
  bool key(const std::string& k, std::vector<uint8_t>& out) const {
    out.clear();
    auto it = kv_.find(k);
    if (it == kv_.end()) return true;
    const std::string& h = it->second;
    if (h.size() != 64) {
      std::fprintf(stderr, "--%s needs 64 hex digits (32 bytes)\n", k.c_str());
      return false;
    }
    for (size_t i = 0; i < 64; i += 2) {
      char* end = nullptr;
      const std::string byte = h.substr(i, 2);
      const long v = std::strtol(byte.c_str(), &end, 16);
      if (*end != '\0') {
        std::fprintf(stderr, "--%s: not hex\n", k.c_str());
        return false;
      }
      out.push_back(static_cast<uint8_t>(v));
    }
    return true;
  }

  // Parses "WxH". Returns false if the key is absent or malformed.
  bool size(const std::string& k, int& w, int& h) const {
    auto it = kv_.find(k);
    return it != kv_.end() && std::sscanf(it->second.c_str(), "%dx%d", &w, &h) == 2;
  }

 private:
  std::map<std::string, std::string> kv_;
  std::string positional_;
};

}  // namespace rdaccel::examples
