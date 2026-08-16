#include <cassert>
#include <cstdint>

#include "CompletionBanner.h"

int main() {
  completion_banner::Timer banner;
  assert(!banner.visible(100));
  assert(!banner.expire(100));

  banner.show(2, 100, 5000);
  assert(banner.visible(100));
  assert(banner.visible(5099));
  assert(!banner.expire(5099));
  assert(!banner.visible(5100));
  assert(banner.expire(5100));
  assert(banner.agent == -1);
  assert(!banner.expire(5101));

  // The signed deadline comparison remains correct across millis() rollover.
  constexpr std::uint32_t near_wrap = 0xFFFFFF00U;
  banner.show(5, near_wrap, 5000);
  assert(banner.visible(near_wrap + 4999U));
  assert(!banner.visible(near_wrap + 5000U));
  assert(banner.expire(near_wrap + 5000U));
}
