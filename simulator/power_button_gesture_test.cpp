#include <cassert>
#include <cstdint>
#include <limits>

#include "PowerButtonGesture.h"

using power_button_gesture::Detector;
using power_button_gesture::Event;

int main() {
  Detector single(500);
  assert(single.release(1000) == Event::None);
  assert(single.poll(1500) == Event::None);  // boundary still accepts click 2
  assert(single.poll(1501) == Event::SingleClick);
  assert(single.poll(2000) == Event::None);

  Detector doubleClick(500);
  assert(doubleClick.release(1000) == Event::None);
  assert(doubleClick.release(1500) == Event::DoubleClick);
  assert(doubleClick.poll(2000) == Event::None);

  Detector tooSlow(500);
  assert(tooSlow.release(1000) == Event::None);
  assert(tooSlow.poll(1501) == Event::SingleClick);
  assert(tooSlow.release(1502) == Event::None);

  Detector cancelled(500);
  assert(cancelled.release(1000) == Event::None);
  cancelled.cancel();
  assert(cancelled.poll(2000) == Event::None);

  Detector rollover(500);
  const std::uint32_t nearWrap =
      std::numeric_limits<std::uint32_t>::max() - 200;
  assert(rollover.release(nearWrap) == Event::None);
  assert(rollover.poll(300) == Event::SingleClick);  // elapsed 501 ms
}
