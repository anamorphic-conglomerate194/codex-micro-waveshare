#include <cassert>
#include <cstdint>
#include <limits>

#include "ConnectionHealth.h"

using connection_health::Input;
using connection_health::Link;
using connection_health::Quota;

namespace {

constexpr std::uint32_t kHostTtlMs = 300000;
constexpr std::uint32_t kQuotaTtlMs = 180000;

connection_health::Result evaluate(const Input& input, std::uint32_t nowMs) {
  return connection_health::evaluate(input, nowMs, kHostTtlMs, kQuotaTtlMs);
}

}  // namespace

int main() {
  connection_health::ConnectionSet connections;
  auto transition = connections.apply(true, 1);
  assert(transition.becameConnected);
  assert(connections.count() == 1);
  transition = connections.apply(true, 1);
  assert(!transition.changed);  // duplicate callback is harmless

  // All three events can arrive before a UI poll. The empty->nonempty edges
  // must remain visible even though the final aggregate count is still one.
  transition = connections.apply(false, 1);
  assert(transition.becameDisconnected);
  transition = connections.apply(true, 2);
  assert(transition.becameConnected);
  assert(connections.count() == 1);

  // A transient companion connection does not create a new host epoch.
  transition = connections.apply(true, 3);
  assert(transition.changed && !transition.becameConnected);
  transition = connections.apply(false, 3);
  assert(transition.changed && !transition.becameDisconnected);
  assert(connections.count() == 1);

  Input input;
  auto result = evaluate(input, 1000);
  assert(result.link == Link::Offline);
  assert(result.quota == Quota::Waiting);

  input.bleConnected = true;
  input.quotaWaitingSinceMs = 1000;
  result = evaluate(input, 2000);
  assert(result.link == Link::BleOnly);
  assert(result.quota == Quota::Waiting);

  input.hostRpcObserved = true;
  input.lastHostRpcAtMs = 2000;
  result = evaluate(input, 2000 + kHostTtlMs);
  assert(result.link == Link::CodexLive);
  result = evaluate(input, 2001 + kHostTtlMs);
  assert(result.link == Link::BleOnly);

  input.quotaAvailable = true;
  input.quotaReceivedAtMs = 400000;
  result = evaluate(input, 400000 + kQuotaTtlMs);
  assert(result.quota == Quota::Fresh);
  result = evaluate(input, 400001 + kQuotaTtlMs);
  assert(result.quota == Quota::Stale);

  // A live Codex link and stale quota companion are independent states.
  input.lastHostRpcAtMs = 500000;
  input.quotaReceivedAtMs = 100000;
  result = evaluate(input, 500000);
  assert(result.link == Link::CodexLive);
  assert(result.quota == Quota::Stale);

  // Boot gets one quota-TTL grace period; BLE reconnects do not restart it.
  input = {};
  input.bleConnected = true;
  input.quotaWaitingSinceMs = 900000;
  result = evaluate(input, 900000 + kQuotaTtlMs);
  assert(result.quota == Quota::Waiting);
  result = evaluate(input, 900001 + kQuotaTtlMs);
  assert(result.quota == Quota::Stale);

  // Unsigned age arithmetic remains correct across millis() rollover.
  input = {};
  input.bleConnected = true;
  input.hostRpcObserved = true;
  input.lastHostRpcAtMs = std::numeric_limits<std::uint32_t>::max() - 1000;
  input.quotaWaitingSinceMs = std::numeric_limits<std::uint32_t>::max() - 1000;
  input.quotaAvailable = true;
  input.quotaReceivedAtMs = std::numeric_limits<std::uint32_t>::max() - 1000;
  result = evaluate(input, 999);
  assert(result.link == Link::CodexLive);
  assert(result.quota == Quota::Fresh);

  input.bleConnected = false;
  result = evaluate(input, 1000);
  assert(result.link == Link::Offline);
  assert(result.quota == Quota::Fresh);

  result = evaluate(input, 400001 + kQuotaTtlMs);
  assert(result.link == Link::Offline);
  assert(result.quota == Quota::Stale);
}
