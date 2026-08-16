// Native, headless, pixel-identical dashboard renderer.
#include <M5GFX.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "DashboardUi.h"

namespace {

dashboard::State previewState(const char* scenario) {
  dashboard::State state;
  state.linkHealth = dashboard::LinkHealth::CodexLive;
  state.batteryPercent = 82;
  state.quotaAvailable = true;
  state.remainingPercent = 82.0f;
  state.resetInSeconds = 4 * 86400 + 3 * 3600;
  state.threads = {{
      {0xE9EEF5, 0.90f, false},  // idle
      {0x3794FF, 1.00f, true},   // thinking
      {0x2DDD72, 1.00f, false},  // complete
      {0xFFB020, 1.00f, false},  // requires input
      {0xFF4D5E, 1.00f, false},  // error
      {0x000000, 0.00f, false},  // unassigned
  }};

  if (std::strcmp(scenario, "ble") == 0) {
    state.linkHealth = dashboard::LinkHealth::BleOnly;
    state.batteryPercent = 61;
    state.charging = true;
    state.docked = true;
  } else if (std::strcmp(scenario, "live-stale") == 0) {
    state.linkHealth = dashboard::LinkHealth::CodexLive;
    state.quotaStale = true;
    state.batteryPercent = 17;
    state.remainingPercent = 19.0f;
    state.resetInSeconds = 17 * 3600 + 22 * 60;
  } else if (std::strcmp(scenario, "offline") == 0) {
    state.linkHealth = dashboard::LinkHealth::Offline;
    state.batteryPercent = 44;
    state.quotaAvailable = false;
    state.remainingPercent = 0.0f;
    state.resetInSeconds = 0;
  } else if (std::strcmp(scenario, "power-hold") == 0) {
    state.powerOverlay = dashboard::PowerOverlay::HoldToPowerOff;
    state.powerHoldProgress = 0.56f;
  } else if (std::strcmp(scenario, "power-off") == 0) {
    state.linkHealth = dashboard::LinkHealth::Offline;
    state.powerOverlay = dashboard::PowerOverlay::PoweringOff;
  }
  return state;
}

bool validScenario(const char* scenario) {
  return std::strcmp(scenario, "live") == 0 ||
         std::strcmp(scenario, "ble") == 0 ||
         std::strcmp(scenario, "live-stale") == 0 ||
         std::strcmp(scenario, "offline") == 0 ||
         std::strcmp(scenario, "power-hold") == 0 ||
         std::strcmp(scenario, "power-off") == 0;
}

bool writePpm(lgfx::LGFX_Sprite& sprite, const char* path) {
  std::FILE* output = std::fopen(path, "wb");
  if (output == nullptr) return false;
  std::fprintf(output, "P6\n%d %d\n255\n", dashboard::kWidth,
               dashboard::kHeight);
  std::vector<lgfx::bgr888_t> pixels(dashboard::kWidth * dashboard::kHeight);
  sprite.readRectRGB(0, 0, dashboard::kWidth, dashboard::kHeight, pixels.data());
  for (const auto& pixel : pixels) {
    const std::uint8_t rgb[3] = {pixel.R8(), pixel.G8(), pixel.B8()};
    std::fwrite(rgb, 1, sizeof(rgb), output);
  }
  return std::fclose(output) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* outputPath = argc > 1 ? argv[1] : "dashboard-preview.ppm";
  const char* scenario = argc > 2 ? argv[2] : "live";
  if (!validScenario(scenario)) {
    std::fprintf(stderr,
                 "Unknown scenario '%s' (use live, ble, live-stale, offline, "
                 "power-hold, or power-off)\n",
                 scenario);
    return 2;
  }
  lgfx::LGFX_Sprite framebuffer;
  framebuffer.setColorDepth(16);
  if (framebuffer.createSprite(dashboard::kWidth, dashboard::kHeight) == nullptr) {
    std::fprintf(stderr, "Could not allocate preview framebuffer\n");
    return 1;
  }
  framebuffer.setTextWrap(false);
  dashboard::render(framebuffer, previewState(scenario));
  if (!writePpm(framebuffer, outputPath)) {
    std::fprintf(stderr, "Could not write %s\n", outputPath);
    return 1;
  }
  std::printf("Rendered %s [%s] (%d x %d, RGB565)\n", outputPath, scenario,
              dashboard::kWidth, dashboard::kHeight);
  return 0;
}
