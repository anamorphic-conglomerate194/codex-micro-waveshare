// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace connection_health {

enum class Link : std::uint8_t {
  Offline,
  BleOnly,
  CodexLive,
};

enum class Quota : std::uint8_t {
  Waiting,
  Fresh,
  Stale,
};

struct Input {
  bool bleConnected = false;
  bool hostRpcObserved = false;
  std::uint32_t lastHostRpcAtMs = 0;
  std::uint32_t quotaWaitingSinceMs = 0;
  bool quotaAvailable = false;
  std::uint32_t quotaReceivedAtMs = 0;
};

struct Result {
  Link link = Link::Offline;
  Quota quota = Quota::Waiting;
};

// Bluedroid can deliver disconnect and reconnect callbacks before the Arduino
// loop gets another turn.  Preserve those ordered conn_id transitions instead
// of sampling only the final aggregate count, which could otherwise make a new
// host session inherit the previous session's CODEX LIVE state.
class ConnectionSet {
 public:
  static constexpr std::size_t kCapacity = 8;

  struct Transition {
    bool changed = false;
    bool becameConnected = false;
    bool becameDisconnected = false;
    bool overflow = false;
  };

  Transition apply(bool connected, std::uint16_t id) {
    const std::size_t before = count();
    if (connected) {
      for (const Slot& slot : slots_) {
        if (slot.active && slot.id == id) return {};
      }
      for (Slot& slot : slots_) {
        if (!slot.active) {
          slot.id = id;
          slot.active = true;
          return transition(before, before + 1);
        }
      }
      Transition result;
      result.overflow = true;
      return result;
    }

    for (Slot& slot : slots_) {
      if (slot.active && slot.id == id) {
        slot.active = false;
        return transition(before, before - 1);
      }
    }
    return {};
  }

  bool contains(std::uint16_t id) const {
    for (const Slot& slot : slots_) {
      if (slot.active && slot.id == id) return true;
    }
    return false;
  }

  std::size_t count() const {
    std::size_t result = 0;
    for (const Slot& slot : slots_) result += slot.active ? 1U : 0U;
    return result;
  }

  bool sameMembers(const ConnectionSet& other) const {
    if (count() != other.count()) return false;
    for (const Slot& slot : slots_) {
      if (slot.active && !other.contains(slot.id)) return false;
    }
    return true;
  }

 private:
  struct Slot {
    std::uint16_t id = 0;
    bool active = false;
  };

  static Transition transition(std::size_t before, std::size_t after) {
    Transition result;
    result.changed = before != after;
    result.becameConnected = before == 0 && after > 0;
    result.becameDisconnected = before > 0 && after == 0;
    return result;
  }

  std::array<Slot, kCapacity> slots_ = {};
};

// Unsigned subtraction deliberately handles millis() rollover.
inline std::uint32_t age(std::uint32_t nowMs, std::uint32_t thenMs) {
  return nowMs - thenMs;
}

inline Result evaluate(const Input& input, std::uint32_t nowMs,
                       std::uint32_t hostRpcTtlMs,
                       std::uint32_t quotaTtlMs) {
  Result result;

  result.link = !input.bleConnected
                    ? Link::Offline
                    : (input.hostRpcObserved &&
                               age(nowMs, input.lastHostRpcAtMs) <= hostRpcTtlMs
                           ? Link::CodexLive
                           : Link::BleOnly);

  if (input.quotaAvailable) {
    result.quota = age(nowMs, input.quotaReceivedAtMs) <= quotaTtlMs
                       ? Quota::Fresh
                       : Quota::Stale;
  } else if (input.bleConnected) {
    result.quota = age(nowMs, input.quotaWaitingSinceMs) <= quotaTtlMs
                       ? Quota::Waiting
                       : Quota::Stale;
  } else {
    result.quota = Quota::Waiting;
  }
  return result;
}

}  // namespace connection_health
