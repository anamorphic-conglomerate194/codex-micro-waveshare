// SPDX-License-Identifier: MIT
#pragma once

#include <M5GFX.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#include "SpaceMonoVlw.h"

namespace dashboard {

constexpr int kWidth = 466;
constexpr int kHeight = 466;
constexpr int kCenterX = 233;
constexpr int kQuotaCenterY = 237;
constexpr int kAgentRadius = 56;
constexpr int kSendRadius = 104;

// Orbital telemetry palette. Keep this compact: status hues come from the Mac,
// while the device chrome stays quiet enough for the quota and alerts to lead.
constexpr std::uint16_t kBackground = 0x0000;    // VOID       #000000
constexpr std::uint16_t kPanel = 0x10C4;         // GRAPHITE   #111820
constexpr std::uint16_t kPanelPressed = 0x1946;  // DEEP STEEL #1D2935
constexpr std::uint16_t kText = 0xEF7E;          // ICE        #E8EEF5
constexpr std::uint16_t kMuted = 0x7C53;         // STEEL      #7D8A98
constexpr std::uint16_t kTrack = 0x2187;         // TRACK      #26323D
constexpr std::uint16_t kAccent = 0x16B6;        // SIGNAL     #12D6B2
constexpr std::uint16_t kVoice = 0x5F17;         // VOICE      #5BE0BD
constexpr std::uint16_t kWarning = 0xFDA9;       // AMBER      #FFB44A
constexpr std::uint16_t kDanger = 0xFAED;        // CORAL      #FF5D6C

enum class AgentStatus : std::uint8_t {
  Unassigned,
  Idle,
  Thinking,
  Complete,
  RequiresInput,
  Error,
  Active,
};

enum class LinkHealth : std::uint8_t {
  Offline,
  BleOnly,
  CodexLive,
};

enum class PowerOverlay : std::uint8_t {
  None,
  HoldToPowerOff,
  PoweringOff,
};

struct ThreadVisual {
  std::uint32_t color = 0;
  float brightness = 0.0f;
  bool focused = false;
};

struct Point {
  std::int16_t x;
  std::int16_t y;
};

struct State {
  std::array<ThreadVisual, 6> threads = {};
  LinkHealth linkHealth = LinkHealth::Offline;
  std::int8_t batteryPercent = -1;
  bool charging = false;
  bool docked = false;
  bool quotaAvailable = false;
  bool quotaStale = false;
  float remainingPercent = 0.0f;
  std::uint32_t resetInSeconds = 0;
  bool leftPressed = false;
  bool rightPressed = false;
  bool sendPressed = false;
  int activeTouchAgent = -1;
  int swipeDirection = -1;
  int completedAgent = -1;
  PowerOverlay powerOverlay = PowerOverlay::None;
  float powerHoldProgress = 0.0f;
};

// Real 466 x 466 framebuffer coordinates, orbit radius 164 around (233, 236).
// The enlarged satellites still nearly touch the central instrument while the
// connection status lives inside the dial, freeing the top edge.
static constexpr std::array<Point, 6> kAgentCenters = {{
    {233, 72},
    {375, 154},
    {375, 318},
    {233, 400},
    {91, 318},
    {91, 154},
}};

inline AgentStatus classify(const ThreadVisual& light) {
  if (light.brightness <= 0.01f) return AgentStatus::Unassigned;

  const int red = (light.color >> 16) & 0xFF;
  const int green = (light.color >> 8) & 0xFF;
  const int blue = light.color & 0xFF;
  const int maximum = std::max(red, std::max(green, blue));
  const int minimum = std::min(red, std::min(green, blue));

  if (maximum - minimum < 48 && maximum > 150) return AgentStatus::Idle;
  if (green > red * 1.12f && green > blue * 1.15f) return AgentStatus::Complete;
  if (blue > red * 1.12f && blue > green * 1.05f) return AgentStatus::Thinking;
  if (red > 150 && green > 70 && green > blue * 1.45f) {
    return AgentStatus::RequiresInput;
  }
  if (red > green * 1.20f && red > blue * 1.20f) return AgentStatus::Error;
  return AgentStatus::Active;
}

inline const char* linkHealthLabel(LinkHealth health) {
  switch (health) {
    case LinkHealth::CodexLive: return "CODEX LIVE";
    case LinkHealth::BleOnly: return "BLE ONLY";
    case LinkHealth::Offline: return "OFFLINE";
  }
  return "OFFLINE";
}

template <typename Surface>
std::uint16_t linkHealthColor(Surface& surface, LinkHealth health) {
  switch (health) {
    case LinkHealth::CodexLive: return kVoice;
    case LinkHealth::BleOnly:
      return surface.color565(66, 146, 245);  // same azure as thinking
    case LinkHealth::Offline: return kDanger;
  }
  return kDanger;
}

inline void formatReset(std::uint32_t seconds, char* output, std::size_t size) {
  if (seconds == 0) {
    std::snprintf(output, size, "RESET --");
    return;
  }
  const std::uint32_t days = seconds / 86400;
  const std::uint32_t hours = (seconds % 86400) / 3600;
  const std::uint32_t minutes = (seconds % 3600) / 60;
  if (days > 0) {
    std::snprintf(output, size, "RESET %luD %02luH",
                  static_cast<unsigned long>(days),
                  static_cast<unsigned long>(hours));
  } else if (hours > 0) {
    std::snprintf(output, size, "RESET %luH %02luM",
                  static_cast<unsigned long>(hours),
                  static_cast<unsigned long>(minutes));
  } else {
    std::snprintf(output, size, "RESET %luM",
                  static_cast<unsigned long>(minutes));
  }
}

template <typename Surface>
std::uint16_t rgb888To565(Surface& surface, std::uint32_t color,
                          float brightness = 1.0f) {
  brightness = std::max(0.0f, std::min(1.0f, brightness));
  const auto red = static_cast<std::uint8_t>(((color >> 16) & 0xFF) * brightness);
  const auto green = static_cast<std::uint8_t>(((color >> 8) & 0xFF) * brightness);
  const auto blue = static_cast<std::uint8_t>((color & 0xFF) * brightness);
  return surface.color565(red, green, blue);
}

template <typename Surface>
void centered(Surface& surface, const char* text, int x, int y,
              std::uint16_t color) {
  surface.setTextDatum(middle_center);
  surface.setTextSize(1.0f);
  surface.setTextColor(color);
  surface.drawString(text, x, y);
}

// Curated status palette. The Mac's raw thread colors are treated as semantic
// input (which status), not display output: hues are regraded here so all six
// keys sit in one family with the dial chrome. Amber and coral reuse the
// kWarning/kDanger hues; unknown custom colors pass through untouched.
inline std::uint32_t statusFillRgb(AgentStatus status,
                                   const ThreadVisual& light) {
  switch (status) {
    case AgentStatus::Idle:          return 0xB7C2CD;  // quiet silver
    case AgentStatus::Thinking:      return 0x4292F5;  // azure
    case AgentStatus::Complete:      return 0x2BC96E;  // spring green
    case AgentStatus::RequiresInput: return 0xF7AC42;  // amber, kWarning family
    case AgentStatus::Error:         return 0xF55A68;  // coral, kDanger family
    default:                         return light.color;
  }
}

template <typename Surface>
void drawAgentShapes(Surface& surface, const State& state) {
  for (int i = 0; i < 6; ++i) {
    const ThreadVisual& light = state.threads[i];
    const AgentStatus status = classify(light);
    const Point point = kAgentCenters[i];
    const bool pressed = state.activeTouchAgent == i;

    if (status == AgentStatus::Unassigned) {
      if (light.focused) {
        surface.fillSmoothCircle(point.x, point.y, kAgentRadius + 4, kText);
      }
      surface.fillSmoothCircle(point.x, point.y, kAgentRadius, kTrack);
      surface.fillSmoothCircle(point.x, point.y, kAgentRadius - 2,
                               pressed ? kPanelPressed : kPanel);
      continue;
    }

    const bool emphasized = status == AgentStatus::Thinking ||
                            status == AgentStatus::Complete ||
                            status == AgentStatus::RequiresInput ||
                            status == AgentStatus::Error;
    const std::uint32_t fillRgb = statusFillRgb(status, light);
    if (emphasized) {
      surface.fillSmoothCircle(
          point.x, point.y, kAgentRadius + 4,
          rgb888To565(surface, fillRgb, light.brightness * 0.16f));
    }

    // Codex supplies the live focus signal. Draw it as a static ice-white ring
    // directly against the key edge; there is deliberately no black gap.
    if (light.focused) {
      surface.fillSmoothCircle(point.x, point.y, kAgentRadius + 4, kText);
    }

    // Solid keys: the full disc carries the status color so state reads at a
    // glance; a pressed key dims instead of swapping to a panel fill. The
    // darker same-hue edge seats the disc into the face like a physical key.
    const float fillBrightness =
        light.brightness * (pressed ? 0.72f : 1.0f);
    surface.fillSmoothCircle(point.x, point.y, kAgentRadius,
                             rgb888To565(surface, fillRgb, fillBrightness * 0.55f));
    surface.fillSmoothCircle(point.x, point.y, kAgentRadius - 4,
                             rgb888To565(surface, fillRgb, fillBrightness));
  }
}

// Label legibility on a solid fill depends on the fill, not the theme: bright
// keys (idle white, amber, green) need dark text, dark keys need ice.
template <typename Surface>
std::uint16_t agentLabelColor(Surface& surface, const ThreadVisual& light,
                              AgentStatus status) {
  if (status == AgentStatus::Unassigned) return kMuted;
  const std::uint32_t fillRgb = statusFillRgb(status, light);
  const int red =
      static_cast<int>(((fillRgb >> 16) & 0xFF) * light.brightness);
  const int green =
      static_cast<int>(((fillRgb >> 8) & 0xFF) * light.brightness);
  const int blue = static_cast<int>((fillRgb & 0xFF) * light.brightness);
  const int luminance = (red * 77 + green * 150 + blue * 29) >> 8;
  return luminance > 150 ? surface.color565(13, 18, 24) : kText;
}

template <typename Surface>
void drawDialShapes(Surface& surface, const State& state) {
  constexpr int outerRadius = 104;
  constexpr int innerRadius = 94;
  surface.fillArc(kCenterX, kQuotaCenterY, outerRadius, innerRadius, 0, 360,
                  kTrack);
  if (state.quotaAvailable) {
    const float remaining =
        std::max(0.0f, std::min(100.0f, state.remainingPercent));
    const std::uint16_t color =
        state.quotaStale
            ? surface.color565(116, 85, 39)
            : (remaining > 20.0f ? kAccent : kWarning);
    surface.fillArc(kCenterX, kQuotaCenterY, outerRadius, innerRadius, 0,
                    static_cast<int>(remaining * 3.6f), color);
  }

  // Four recessed cuts make the progress ring read like calibrated telemetry.
  surface.fillSmoothRoundRect(kCenterX - 2, kQuotaCenterY - outerRadius, 4, 12,
                              2, kBackground);
  surface.fillSmoothRoundRect(kCenterX + innerRadius - 2, kQuotaCenterY - 2,
                              12, 4, 2, kBackground);
  surface.fillSmoothRoundRect(kCenterX - 2, kQuotaCenterY + innerRadius - 2, 4,
                              12, 2, kBackground);
  surface.fillSmoothRoundRect(kCenterX - outerRadius, kQuotaCenterY - 2, 12, 4,
                              2, kBackground);

  // The inner hairline makes the whole dial discoverable as the Send key.
  surface.fillSmoothCircle(kCenterX, kQuotaCenterY, innerRadius - 3, kTrack);
  surface.fillSmoothCircle(kCenterX, kQuotaCenterY, innerRadius - 6,
                           state.sendPressed ? kPanelPressed : kBackground);
}

template <typename Surface>
void drawSmallText(Surface& surface, const State& state) {
  surface.loadFont(font_data::kSpaceMono18Vlw);

  // The health word is the status light: color and copy distinguish a live
  // Codex session from a bare BLE link, a stale companion, and no link at all.
  // Keeping it inside the dial leaves the six edge keys at their full size.
  centered(surface, linkHealthLabel(state.linkHealth), kCenterX,
           kQuotaCenterY - 69, linkHealthColor(surface, state.linkHealth));

  const char* dialLabel =
      state.quotaStale
          ? "SYNC STALE"
          : (state.linkHealth == LinkHealth::Offline ? "WAITING CODEX"
                                                     : "LEFT");
  const std::uint16_t dialLabelColor = state.quotaStale ? kWarning : kMuted;
  centered(surface, dialLabel, kCenterX, kQuotaCenterY - 39, dialLabelColor);

  char reset[24];
  formatReset(state.resetInSeconds, reset, sizeof(reset));
  const char* resetLabel = state.quotaAvailable
                               ? reset
                               : (state.quotaStale ? "CHECK COMPANION"
                                                   : "NO QUOTA DATA");
  const std::uint16_t resetColor =
      state.quotaStale ? kMuted : (state.quotaAvailable ? kText : kMuted);
  centered(surface, resetLabel, kCenterX, kQuotaCenterY + 43, resetColor);

  // Compact battery telemetry at the foot of the dial. A bright bolt is the
  // charging affordance; percentage remains readable without relying on color.
  constexpr int kBatteryX = kCenterX - 38;
  constexpr int kBatteryY = kQuotaCenterY + 62;
  constexpr int kBatteryWidth = 29;
  constexpr int kBatteryHeight = 15;
  const int battery = std::max(-1, std::min(100,
      static_cast<int>(state.batteryPercent)));
  const std::uint16_t batteryColor =
      battery < 0 ? kMuted
                  : (state.docked ? kAccent
                                  : (battery <= 20 ? kWarning : kText));

  surface.fillSmoothRoundRect(kBatteryX, kBatteryY, kBatteryWidth,
                              kBatteryHeight, 3, batteryColor);
  surface.fillSmoothRoundRect(kBatteryX + 2, kBatteryY + 2,
                              kBatteryWidth - 4, kBatteryHeight - 4, 2,
                              kBackground);
  surface.fillSmoothRoundRect(kBatteryX + kBatteryWidth, kBatteryY + 4, 3,
                              kBatteryHeight - 8, 1, batteryColor);
  if (battery > 0) {
    const int fillWidth = std::max(2, (kBatteryWidth - 6) * battery / 100);
    surface.fillSmoothRoundRect(kBatteryX + 3, kBatteryY + 3, fillWidth,
                                kBatteryHeight - 6, 1, batteryColor);
  }
  if (state.charging) {
    const int boltX = kBatteryX + kBatteryWidth / 2;
    surface.fillTriangle(boltX, kBatteryY + 1, boltX - 5, kBatteryY + 8,
                         boltX - 1, kBatteryY + 8, kText);
    surface.fillTriangle(boltX - 1, kBatteryY + 7, boltX + 4,
                         kBatteryY + 7, boltX - 3,
                         kBatteryY + kBatteryHeight - 1, kText);
  }

  char batteryLabel[12];
  if (battery < 0) {
    std::snprintf(batteryLabel, sizeof(batteryLabel), "--%%");
  } else {
    std::snprintf(batteryLabel, sizeof(batteryLabel), "%d%%", battery);
  }
  centered(surface, batteryLabel, kCenterX + 21,
           kBatteryY + kBatteryHeight / 2 + 1, batteryColor);

  for (int i = 0; i < 6; ++i) {
    char label[4];
    std::snprintf(label, sizeof(label), "A%d", i + 1);
    const AgentStatus status = classify(state.threads[i]);
    centered(surface, label, kAgentCenters[i].x, kAgentCenters[i].y - 3,
             agentLabelColor(surface, state.threads[i], status));
  }
  surface.unloadFont();
}

template <typename Surface>
void drawQuotaValue(Surface& surface, const State& state) {
  surface.loadFont(font_data::kSpaceMono46Vlw);
  char value[8];
  if (state.quotaAvailable) {
    std::snprintf(value, sizeof(value), "%.0f%%",
                  std::max(0.0f, std::min(100.0f, state.remainingPercent)));
    centered(surface, value, kCenterX, kQuotaCenterY + 1,
             state.quotaStale ? kMuted : kText);
  } else {
    centered(surface, "--", kCenterX, kQuotaCenterY + 1, kMuted);
  }
  surface.unloadFont();
}

template <typename Surface>
void drawTransient(Surface& surface, const State& state) {
  const char* message = nullptr;
  std::uint16_t color = kVoice;
  char completed[12];
  if (state.sendPressed) {
    message = "SEND";
    color = kText;
  } else if (state.leftPressed) {
    message = "LISTENING";
    color = kAccent;
  } else if (state.rightPressed) {
    message = "VOICE CHAT";
  } else if (state.swipeDirection >= 0 && state.swipeDirection < 4) {
    static constexpr const char* kSwipeMessages[] = {
        "STICK / UP", "STICK / RIGHT", "STICK / DOWN", "STICK / LEFT"};
    message = kSwipeMessages[state.swipeDirection];
    color = kAccent;
  } else if (state.completedAgent >= 0) {
    std::snprintf(completed, sizeof(completed), "A%d DONE",
                  state.completedAgent + 1);
    message = completed;
  }
  if (message == nullptr) return;

  surface.fillSmoothRoundRect(kCenterX - 102, kQuotaCenterY - 32, 204, 64, 18,
                              kBackground);
  surface.fillSmoothRoundRect(kCenterX - 102, kQuotaCenterY - 32, 204, 64, 18,
                              color);
  surface.fillSmoothRoundRect(kCenterX - 98, kQuotaCenterY - 28, 196, 56, 15,
                              kPanel);
  surface.loadFont(font_data::kSpaceMono18Vlw);
  centered(surface, message, kCenterX, kQuotaCenterY + 1, color);
  surface.unloadFont();
}

template <typename Surface>
void drawPowerOverlay(Surface& surface, const State& state) {
  if (state.powerOverlay == PowerOverlay::None) return;

  const bool confirming =
      state.powerOverlay == PowerOverlay::HoldToPowerOff;
  const char* heading = confirming ? "KEEP HOLDING" : "POWERING OFF";
  const char* detail = confirming ? "ALERTS PAUSE" : "RED OR USB WAKE";
  const std::uint16_t color = confirming ? kWarning : kDanger;

  // Power state owns the dial interior. This completely masks the stale quota
  // copy without reaching into the surrounding agent keys.
  constexpr int kOverlayRadius = 104;
  constexpr int kOverlayInnerRadius = 94;
  surface.fillSmoothCircle(kCenterX, kQuotaCenterY, kOverlayRadius, kTrack);
  const float progress = confirming
                             ? std::max(0.0f, std::min(1.0f,
                                   state.powerHoldProgress))
                             : 1.0f;
  surface.fillArc(kCenterX, kQuotaCenterY, kOverlayRadius,
                  kOverlayInnerRadius, 0,
                  static_cast<int>(progress * 360.0f), color);
  surface.fillSmoothCircle(kCenterX, kQuotaCenterY,
                           kOverlayInnerRadius - 3, kPanel);
  surface.loadFont(font_data::kSpaceMono18Vlw);
  centered(surface, heading, kCenterX, kQuotaCenterY - 13, color);
  centered(surface, detail, kCenterX, kQuotaCenterY + 17, kText);
  surface.unloadFont();
}

template <typename Surface>
void render(Surface& surface, const State& state) {
  surface.fillScreen(kBackground);
  drawDialShapes(surface, state);
  drawAgentShapes(surface, state);
  drawSmallText(surface, state);
  drawQuotaValue(surface, state);
  drawTransient(surface, state);
  drawPowerOverlay(surface, state);
}

inline int agentAtPoint(int x, int y) {
  for (int i = 0; i < 6; ++i) {
    const int dx = x - kAgentCenters[i].x;
    const int dy = y - kAgentCenters[i].y;
    if (dx * dx + dy * dy <=
        (kAgentRadius + 4) * (kAgentRadius + 4)) {
      return i;
    }
  }
  return -1;
}

inline bool sendAtPoint(int x, int y) {
  const int dx = x - kCenterX;
  const int dy = y - kQuotaCenterY;
  return dx * dx + dy * dy <= kSendRadius * kSendRadius;
}

}  // namespace dashboard
