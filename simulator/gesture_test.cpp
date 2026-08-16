#include <cassert>
#include <cmath>

#include "TouchGesture.h"

using touch_gesture::Direction;

int main() {
  constexpr int threshold = 52;

  assert(touch_gesture::classifySwipe(0, 0, threshold) == Direction::None);
  assert(touch_gesture::classifySwipe(51, 0, threshold) == Direction::None);
  assert(touch_gesture::classifySwipe(52, 0, threshold) == Direction::Right);
  assert(touch_gesture::classifySwipe(-70, 5, threshold) == Direction::Left);
  assert(touch_gesture::classifySwipe(4, -80, threshold) == Direction::Up);
  assert(touch_gesture::classifySwipe(-6, 80, threshold) == Direction::Down);

  // Diagonal gestures lock to the dominant axis; exact ties prefer horizontal.
  assert(touch_gesture::classifySwipe(70, -55, threshold) == Direction::Right);
  assert(touch_gesture::classifySwipe(-55, -70, threshold) == Direction::Up);
  assert(touch_gesture::classifySwipe(-60, 60, threshold) == Direction::Left);

  assert(std::fabs(touch_gesture::normalizedAngle(Direction::Right) - 0.00f) <
         0.001f);
  assert(std::fabs(touch_gesture::normalizedAngle(Direction::Down) - 0.25f) <
         0.001f);
  assert(std::fabs(touch_gesture::normalizedAngle(Direction::Left) - 0.50f) <
         0.001f);
  assert(std::fabs(touch_gesture::normalizedAngle(Direction::Up) - 0.75f) <
         0.001f);
}
