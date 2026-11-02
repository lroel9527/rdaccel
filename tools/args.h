#pragma once

// Tiny --key=value command line parser shared by the tools.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace rdaccel::tools {

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
  // Parses "WxH". Returns false if the key is absent or malformed.
  bool size(const std::string& k, int& w, int& h) const {
    auto it = kv_.find(k);
    return it != kv_.end() && std::sscanf(it->second.c_str(), "%dx%d", &w, &h) == 2;
  }

 private:
  std::map<std::string, std::string> kv_;
  std::string positional_;
};

}  // namespace rdaccel::tools
