// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace power_button_gesture {

enum class Event : std::uint8_t {
  None,
  SingleClick,
  DoubleClick,
};

// A release-based detector lets the application keep the PM1 long-hold
// Download gesture untouched. Unsigned subtraction deliberately handles
// millis() rollover.
class Detector {
 public:
  explicit constexpr Detector(std::uint32_t doubleClickWindowMs)
      : doubleClickWindowMs_(doubleClickWindowMs) {}

  Event release(std::uint32_t nowMs) {
    if (pending_ && nowMs - firstReleaseAtMs_ <= doubleClickWindowMs_) {
      pending_ = false;
      return Event::DoubleClick;
    }
    pending_ = true;
    firstReleaseAtMs_ = nowMs;
    return Event::None;
  }

  Event poll(std::uint32_t nowMs) {
    if (!pending_ || nowMs - firstReleaseAtMs_ <= doubleClickWindowMs_) {
      return Event::None;
    }
    pending_ = false;
    return Event::SingleClick;
  }

  void cancel() { pending_ = false; }

 private:
  std::uint32_t doubleClickWindowMs_ = 0;
  std::uint32_t firstReleaseAtMs_ = 0;
  bool pending_ = false;
};

}  // namespace power_button_gesture
