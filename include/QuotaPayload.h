// SPDX-License-Identifier: MIT
#pragma once

#include <ArduinoJson.h>

#include <cmath>
#include <cstdint>

namespace quota_payload {

struct Snapshot {
  float remainingPercent = 0.0f;
  std::uint32_t resetInSeconds = 0;
};

inline bool parse(JsonObjectConst value, Snapshot& output) {
  const JsonVariantConst remaining = value["remaining_percent"];
  const JsonVariantConst reset = value["reset_in_seconds"];
  if (!remaining.is<float>() || !reset.is<std::uint32_t>()) return false;

  const float percent = remaining.as<float>();
  if (!std::isfinite(percent) || percent < 0.0f || percent > 100.0f) {
    return false;
  }
  output.remainingPercent = percent;
  output.resetInSeconds = reset.as<std::uint32_t>();
  return true;
}

}  // namespace quota_payload
