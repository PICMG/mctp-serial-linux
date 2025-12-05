// Minimal test logging helper (previously removed). Provides a simple
// `util::setVerbose(bool)` used by the in-tree test runner.
#pragma once
#include <atomic>

namespace util {

inline void setVerbose(bool v) {
  static std::atomic<bool> verb{false};
  verb.store(v);
}

inline bool isVerbose() {
  static std::atomic<bool> verb{false};
  return verb.load();
}

} // namespace util
