// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace touch_gesture {

enum class Direction : std::int8_t {
  None = -1,
  Up = 0,
  Right = 1,
  Down = 2,
  Left = 3,
};

inline Direction classifySwipe(int dx, int dy, int threshold) {
  const std::int64_t distanceSquared =
      static_cast<std::int64_t>(dx) * dx +
      static_cast<std::int64_t>(dy) * dy;
  const std::int64_t thresholdSquared =
      static_cast<std::int64_t>(threshold) * threshold;
  if (distanceSquared < thresholdSquared) return Direction::None;

  const int absoluteX = dx < 0 ? -dx : dx;
  const int absoluteY = dy < 0 ? -dy : dy;
  if (absoluteX >= absoluteY) {
    return dx >= 0 ? Direction::Right : Direction::Left;
  }
  return dy >= 0 ? Direction::Down : Direction::Up;
}

inline float normalizedAngle(Direction direction) {
  switch (direction) {
    case Direction::Right:
      return 0.00f;
    case Direction::Down:
      return 0.25f;
    case Direction::Left:
      return 0.50f;
    case Direction::Up:
      return 0.75f;
    case Direction::None:
      return 0.00f;
  }
  return 0.00f;
}

inline const char* name(Direction direction) {
  switch (direction) {
    case Direction::Up:
      return "UP";
    case Direction::Right:
      return "RIGHT";
    case Direction::Down:
      return "DOWN";
    case Direction::Left:
      return "LEFT";
    case Direction::None:
      return "NONE";
  }
  return "NONE";
}

}  // namespace touch_gesture
