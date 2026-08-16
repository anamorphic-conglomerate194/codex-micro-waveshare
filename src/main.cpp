// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo
// StopWatch port changes copyright (c) 2026 Codex Micro for StopWatch contributors

#include <Arduino.h>
#include <M5PM1.h>
#include <M5Unified.h>

#include <array>
#include <cmath>

#include "CodexMicroBle.h"
#include "CompletionBanner.h"
#include "ConnectionHealth.h"
#include "DashboardUi.h"
#include "PowerButtonGesture.h"
#include "TouchGesture.h"
// CODEX_STOPWATCH_THIRD_PARTY: AXP2101 PMIC on Waveshare 1.75C (replaces M5PM1).
#include "Axp2101Power.h"
// CODEX_STOPWATCH_THIRD_PARTY: ES8311 audio on Waveshare 1.75C.
#include "BoardAudio.h"
#if defined(CODEX_STOPWATCH_USB_MIC)
#include "UsbMic.h"
#endif

namespace {

constexpr uint32_t kQuotaStaleAfterMs = 180000;
constexpr uint32_t kHostRpcLiveForMs = 300000;
constexpr uint32_t kCompletionBannerMs = 5000;
constexpr uint32_t kVoiceTapBannerMs = 900;
constexpr uint32_t kVoiceClickPulseMs = 70;
constexpr uint32_t kTouchKeyPulseMs = 55;
// CODEX_STOPWATCH_THIRD_PARTY: raised from upstream's 52 to 100. On the
// Waveshare 1.75C the touch digitizer jitters enough that a tap on the large
// center Send dial (radius 104) often drifts past 52px, which the swipe
// classifier stole as a joystick gesture and cancelled the Send. 100px still
// registers deliberate swipes while keeping center taps stable.
constexpr int kSwipeThresholdPx = 100;
constexpr uint8_t kButtonHapticIntensity = 176;
// 144 (56% PWM duty) sat at the motor's start threshold and spun up only
// sometimes; 176 matches the physical buttons, which never felt flaky.
constexpr uint8_t kTouchHapticIntensity = 176;
constexpr uint8_t kSwipeHapticIntensity = 168;
constexpr uint32_t kWakeHapticDurationMs = 20;
constexpr uint32_t kButtonHapticDurationMs = 42;
constexpr uint32_t kTouchHapticDurationMs = 32;
constexpr uint32_t kSwipeHapticDurationMs = 40;
constexpr uint32_t kBatteryDimAfterMs = 120000;
constexpr uint32_t kBatteryDeskSleepAfterMs = 300000;
constexpr uint32_t kDockDimAfterMs = 600000;
constexpr uint32_t kDockDeskSleepAfterMs = 1800000;
constexpr uint32_t kPowerTelemetryIntervalMs = 2000;
constexpr uint32_t kBatteryTelemetryIntervalMs = 30000;
constexpr uint32_t kPowerHoldPromptMs = 2000;
constexpr uint32_t kTravelPowerOffMs = 6000;
constexpr uint32_t kPowerButtonDoubleClickMs = 500;
// CODEX_STOPWATCH_THIRD_PARTY: Waveshare 1.75C has no physical side keys.
// Long-press the left/right edge bands to emulate them. Screen is 466x466;
// the central Send dial spans x in [129,337], so bands at |x-233|>=117
// (x<=116 / x>=349) never overlap it. 500ms sits above tap and below the
// 2000ms power-hold menu.
constexpr uint32_t kSideHoldTriggerMs = 500;
constexpr int kSideBandHalfWidth = 117;      // |x - 233| >= 117 selects the band
constexpr int kSideBandJitterPx = 24;        // allow finger drift while held
// CODEX_STOPWATCH_THIRD_PARTY: Waveshare 1.75C board buttons. BOOT is the
// standard GPIO0 download-mode button (active low, pulled up); PWR is wired
// to the AXP2101 PEK input and is polled through axp::pekPress().
constexpr int kBootButtonPin = 0;
constexpr uint32_t kBootButtonDebounceMs = 40;
constexpr uint8_t kActiveBrightness = 120;
constexpr uint8_t kDimBrightness = 24;
// M5Unified's IOExpander_Base API is zero-based: physical M5IOE1 G8 is 7,
// while physical G5 is 4.
constexpr uint8_t kIoeSharedL3bEnable = 7;
constexpr uint8_t kIoePanelReset = 4;
#if defined(CODEX_STOPWATCH_USB_MIC)
// G8 powers the shared L3B rail, including the microphone path. The optional
// USB-mic image therefore keeps G8 on and uses brightness-only desk sleep.
constexpr bool kCutSharedRailInDeskSleep = false;
#else
// The default wireless image can remove the AMOLED/audio/motor L3B rail while
// leaving the ESP32, touch controller, PM1, and BLE transport alive.
constexpr bool kCutSharedRailInDeskSleep = true;
#endif
#if !defined(CODEX_STOPWATCH_USB_MIC)
constexpr uint32_t kCompletionChimeSampleRate = 12000;
constexpr uint32_t kCompletionChimeDurationMs = 270;
constexpr size_t kCompletionChimeSamples =
    kCompletionChimeSampleRate * kCompletionChimeDurationMs / 1000;
constexpr float kPi = 3.14159265358979323846f;
constexpr uint8_t kCompletionChimeVolume = 160;
#endif

constexpr char kLeftMicSwitch[] = "ACT10";
// ChatGPT 26.727 exposes one combined Mic key but no independent ACT10/ACT11
// switch setting. Use the fourth configurable Command Key for Voice Chat.
constexpr char kVoiceChatCommandKey[] = "ACT09";
constexpr char kSendKey[] = "ACT12";
const char* kAgentKeys[] = {"AG00", "AG01", "AG02", "AG03", "AG04", "AG05"};

CodexMicroBle codex;
CodexMicroState state;
M5Canvas canvas(&M5.Display);
M5PM1 powerManager;
uint32_t lastDrawMs = 0;
uint32_t lastBatteryMs = 0;
uint32_t lastPowerTelemetryMs = 0;
uint32_t quotaWaitingSinceMs = 0;
bool leftPressed = false;
bool rightPressed = false;
bool touchAgentPressed = false;
bool touchSendPressed = false;
bool touchTracking = false;
bool voiceTapBannerVisible = false;
bool voiceClickReleasePending = false;
// CODEX_STOPWATCH_THIRD_PARTY: emulated left/right side keys via long-press.
bool touchSideLeftActive = false;   // left band long-press -> Push to talk
bool touchSideRightActive = false;  // right band long-press -> Voice chat
// CODEX_STOPWATCH_THIRD_PARTY: Waveshare board buttons (BOOT=GPIO0 PTT,
// PWR=AXP2101 PEK voice-chat toggle).
bool bootButtonPressed = false;
uint32_t bootButtonChangedAtMs = 0;
int8_t activeTouchAgent = -1;
int16_t touchStartX = 0;
int16_t touchStartY = 0;
int16_t lastDbgTx = -1;
int16_t lastDbgTy = -1;
// CODEX_STOPWATCH_THIRD_PARTY: the 1.75C CST9217 touchscreen emits phantom
// taps (same spot, fast press/release cycles) when the watch rests on a desk.
// Ignore taps shorter than this so noise does not fire Agent/Send keys, but
// keep it short so normal quick taps still register.
uint32_t touchBeganAtMs = 0;
constexpr uint32_t kMinTapHoldMs = 20;
// Repeat-suppression: if the panel floods the same keys with phantom taps
// (>=4 commits within 2s), treat it as desk noise and ignore touches for 3s.
uint32_t lastTouchCommitAtMs = 0;
int touchCommitCount = 0;
uint32_t touchSuppressUntilMs = 0;
uint32_t touchLockoutUntilMs = 0;
// Spot of the last committed tap; used to tell phantom repeats (same spot)
// from a real new tap (different spot) during the lockout window.
int16_t lastCommitSpotX = -1;
int16_t lastCommitSpotY = -1;
constexpr uint32_t kNoiseBurstWindowMs = 2000;
constexpr int kNoiseBurstThreshold = 4;
constexpr uint32_t kNoiseSuppressMs = 3000;
// CODEX_STOPWATCH_THIRD_PARTY: the CST9217 on the Waveshare 1.75C can emit
// several quick press/release cycles for ONE physical tap. Each cycle used to
// commit a full Agent/Send key, so one tap fired the ChatGPT window repeatedly
// (agent kept flashing + windows kept popping). After any commit, a short
// lockout swallows trailing press/release cycles AT THE SAME SPOT; a press at
// a different spot (another agent, the center Send dial) is a real new tap
// and is allowed through immediately.
constexpr uint32_t kTouchCommitLockoutMs = 600;
constexpr int kPhantomSameSpotPx = 100;

// Returns true when the tap should be ignored as noise (already counted it).
bool suppressNoiseBurst() {
  const uint32_t now = millis();
  if (now < touchSuppressUntilMs) {
    return true;
  }
  if (now - lastTouchCommitAtMs > kNoiseBurstWindowMs) {
    touchCommitCount = 0;
  }
  touchCommitCount++;
  lastTouchCommitAtMs = now;
  if (touchCommitCount >= kNoiseBurstThreshold) {
    touchSuppressUntilMs = now + kNoiseSuppressMs;
    touchCommitCount = 0;
    Serial.println("TOUCH noise burst suppressed");
    return true;
  }
  return false;
}
touch_gesture::Direction activeSwipe = touch_gesture::Direction::None;
uint32_t voiceTapBannerUntilMs = 0;
uint32_t voiceClickReleaseAtMs = 0;
uint32_t rightPressedAtMs = 0;
uint32_t touchSendStartedAtMs = 0;
uint32_t lastPowerOverlayDrawMs = 0;
std::array<dashboard::AgentStatus, 6> previousAgentStatuses = {};
std::array<bool, 6> agentStatusObserved = {};
completion_banner::Timer completionBanner;
#if !defined(CODEX_STOPWATCH_USB_MIC)
std::array<int16_t, kCompletionChimeSamples> completionChimePcm = {};
#endif
bool hapticActive = false;
uint32_t hapticUntilMs = 0;
uint32_t lastActivityMs = 0;
uint8_t appliedBrightness = kActiveBrightness;
int8_t batteryPercent = -1;
bool charging = false;
bool docked = false;
bool dockStateInitialized = false;
bool pendingDockState = false;
uint8_t pendingDockSamples = 0;
bool deskSleeping = false;
bool displayRailOff = false;
bool speakerSuspended = false;
bool powerManagerReady = false;
// CODEX_STOPWATCH_THIRD_PARTY: AXP2101 availability on Waveshare 1.75C.
bool axpReady = false;
bool powerButtonPollingReady = false;
bool powerButtonPressed = false;
bool powerButtonWokeDisplay = false;
bool pendingPowerClickWokeDisplay = false;
power_button_gesture::Detector powerButtonGesture(kPowerButtonDoubleClickMs);
bool touchPowerHoldConsumed = false;
dashboard::PowerOverlay powerOverlay = dashboard::PowerOverlay::None;
bool renderedHealthValid = false;
dashboard::LinkHealth renderedLinkHealth = dashboard::LinkHealth::Offline;
bool renderedQuotaStale = false;
int8_t renderedBatteryPercent = -1;
bool renderedCharging = false;
bool renderedDocked = false;

void drawScreen();
void stopHaptic();
void pressMicSwitch(const char* key, bool& pressed);
void releaseMicSwitch(const char* key, bool& pressed);
void beginVoiceChatClick();
void enterDeskSleep();
void wakeDeskSleep();
[[noreturn]] void enterTravelPowerOff();

void setPanelRail(bool enabled) {
  // CODEX_STOPWATCH_THIRD_PARTY: third-party C152-compatible boards lack
  // M5IOE1; without it the shared AMOLED/audio rail control is a no-op.
  if (!powerManagerReady) return;
  auto& ioe1 = M5.getIOExpander(0);
  // Official C152 mapping: M5IOE1 G8 is PYB_L3B_EN. G4 is touch reset;
  // pulling G4 low here would make a sleeping display impossible to touch-wake.
  ioe1.setHighImpedance(kIoeSharedL3bEnable, false);
  ioe1.setDirection(kIoeSharedL3bEnable, true);
  ioe1.digitalWrite(kIoeSharedL3bEnable, enabled);
}

void setPanelReset(bool high) {
  // CODEX_STOPWATCH_THIRD_PARTY: no M5IOE1 on third-party boards.
  if (!powerManagerReady) return;
  auto& ioe1 = M5.getIOExpander(0);
  // Official C152 mapping: M5IOE1 G5 is CO5300 reset.
  ioe1.setHighImpedance(kIoePanelReset, false);
  ioe1.setDirection(kIoePanelReset, true);
  ioe1.digitalWrite(kIoePanelReset, high);
}

void enterDeskSleep() {
  if (deskSleeping || leftPressed || rightPressed || touchTracking) return;
  stopHaptic();
#if !defined(CODEX_STOPWATCH_USB_MIC)
  speakerSuspended = M5.Speaker.isRunning();
  if (speakerSuspended) M5.Speaker.end();
  // CODEX_STOPWATCH_THIRD_PARTY: Waveshare 1.75C PA is a plain GPIO.
  if (!powerManagerReady) board_audio::setSpeakerAmp(false);
#endif
  M5.Display.setBrightness(0);
#if !defined(CODEX_STOPWATCH_USB_MIC)
  // The USB microphone build keeps the shared AMOLED/audio rail powered and
  // uses brightness-only desk sleep. The CO5300 Sleep Out path through the
  // framebuffer wrapper is not reliable enough on C152 to use while docked.
  M5.Display.sleep();
  M5.Display.waitDisplay();
#endif
  // CODEX_STOPWATCH_THIRD_PARTY: only cut the shared rail when a real
  // PM1/IOE1 power rail exists (C152). Waveshare 1.75C has neither, so it
  // degrades to a brightness-zero + CO5300 Sleep In desk sleep with the
  // QSPI bus and BLE kept alive (same profile as the USB-mic build).
  const bool cutRail = kCutSharedRailInDeskSleep && powerManagerReady;
  if (cutRail) {
    // The repository-owned M5GFX patch forwards sleep to the physical AMOLED
    // and allows a repeat controller init while preserving its framebuffer.
    M5.Display.releaseBus();
    setPanelRail(false);
    displayRailOff = true;
  }
  appliedBrightness = 0;
  deskSleeping = true;
  Serial.printf("POWER desk_sleep dock=%d hard_panel=%d\n", docked ? 1 : 0,
                displayRailOff ? 1 : 0);
}

void wakeDeskSleep() {
  if (!deskSleeping) return;
  if (displayRailOff) {
    setPanelRail(true);
    delay(10);
    setPanelReset(false);
    delay(8);
    setPanelReset(true);
    delay(2);
    // Panel_AMOLED_Framebuffer::init() is patched to forward repeat init to
    // the physical CO5300. This replays the pinned StopWatch command list and
    // re-initializes the released QSPI bus without reallocating the canvas.
    if (!M5.Display.getPanel()->init(true)) {
      Serial.println("POWER panel reinit failed");
    }
    displayRailOff = false;
  }
#if !defined(CODEX_STOPWATCH_USB_MIC)
  else {
    M5.Display.wakeup();
  }
#endif
  M5.Display.setRotation(0);
  M5.Display.setTextWrap(false);
  appliedBrightness = kActiveBrightness;
  M5.Display.setBrightness(appliedBrightness);
#if !defined(CODEX_STOPWATCH_USB_MIC)
  if (speakerSuspended) {
    // CODEX_STOPWATCH_THIRD_PARTY: re-enable the Waveshare PA GPIO before
    // resuming the speaker task.
    if (!powerManagerReady) board_audio::setSpeakerAmp(true);
    if (!M5.Speaker.begin()) {
      Serial.println("POWER speaker resume failed");
    }
    M5.Speaker.setVolume(kCompletionChimeVolume);
    speakerSuspended = false;
  }
#endif
  deskSleeping = false;
  drawScreen();
  Serial.println("POWER desk_wake");
}

#if !defined(CODEX_STOPWATCH_USB_MIC)
float softNoteEnvelope(float noteTime, float noteDuration) {
  constexpr float kAttackSeconds = 0.038f;
  constexpr float kReleaseSeconds = 0.100f;
  if (noteTime < 0.0f || noteTime >= noteDuration) return 0.0f;

  float envelope = 1.0f;
  if (noteTime < kAttackSeconds) {
    const float phase = noteTime / kAttackSeconds;
    const float fade = sinf(phase * kPi * 0.5f);
    envelope *= fade * fade;
  }
  const float releaseStart = noteDuration - kReleaseSeconds;
  if (noteTime > releaseStart) {
    const float phase = (noteTime - releaseStart) / kReleaseSeconds;
    const float fade = cosf(phase * kPi * 0.5f);
    envelope *= fade * fade;
  }
  return envelope;
}

void buildCompletionChime() {
  // A subdued A4 -> C#5 major third with rounded attacks and long releases.
  // Keeping this as one PCM buffer avoids the hard edge between two tone() calls.
  constexpr float kFirstFrequency = 440.00f;
  constexpr float kSecondFrequency = 554.37f;
  constexpr float kFirstStart = 0.0f;
  constexpr float kFirstDuration = 0.145f;
  constexpr float kSecondStart = 0.105f;
  constexpr float kSecondDuration = 0.165f;
  constexpr float kPeakAmplitude = 0.135f * 32767.0f;

  for (size_t sample = 0; sample < completionChimePcm.size(); ++sample) {
    const float time = static_cast<float>(sample) / kCompletionChimeSampleRate;
    const float firstTime = time - kFirstStart;
    const float secondTime = time - kSecondStart;
    const float first =
        sinf(2.0f * kPi * kFirstFrequency * firstTime) *
        softNoteEnvelope(firstTime, kFirstDuration);
    const float second =
        sinf(2.0f * kPi * kSecondFrequency * secondTime) *
        softNoteEnvelope(secondTime, kSecondDuration);
    completionChimePcm[sample] =
        static_cast<int16_t>(kPeakAmplitude * (first + second));
  }
}
#endif

void startHaptic(uint8_t intensity, uint32_t durationMs) {
  M5.Power.setVibration(intensity);
  hapticActive = true;
  hapticUntilMs = millis() + durationMs;
}

void stopHaptic() {
  M5.Power.setVibration(0);
  hapticActive = false;
}

void updateHaptic() {
  if (!hapticActive || static_cast<int32_t>(millis() - hapticUntilMs) < 0) return;
  stopHaptic();
}

// Returns false when the screen was fully off, so the waking tap can be
// swallowed instead of acting on a control the user could not see.
bool noteActivity() {
  // Any non-power interaction after the first red click means the user did
  // not intend to put the desk to sleep. A second red click still reaches the
  // detector because red-button polling does not call noteActivity().
  powerButtonGesture.cancel();
  pendingPowerClickWokeDisplay = false;
  lastActivityMs = millis();
  if (deskSleeping) {
    wakeDeskSleep();
    return false;
  }
  if (appliedBrightness == kActiveBrightness) return true;
  const bool wasOff = appliedBrightness == 0;
  appliedBrightness = kActiveBrightness;
  M5.Display.setBrightness(kActiveBrightness);
  if (wasOff) drawScreen();  // redraws were paused while the panel was dark
  return !wasOff;
}

void updateIdleDimming() {
  // A held mic button or an ongoing touch counts as continuous use.
  if (leftPressed || rightPressed || touchTracking) lastActivityMs = millis();
  const uint32_t idleMs = millis() - lastActivityMs;
  const uint32_t dimAfterMs = docked ? kDockDimAfterMs : kBatteryDimAfterMs;
  const uint32_t sleepAfterMs =
      docked ? kDockDeskSleepAfterMs : kBatteryDeskSleepAfterMs;
  if (idleMs >= sleepAfterMs) {
    enterDeskSleep();
    return;
  }
  if (deskSleeping) return;
  const uint8_t target =
      idleMs >= dimAfterMs ? kDimBrightness : kActiveBrightness;
  if (target != appliedBrightness) {
    appliedBrightness = target;
    M5.Display.setBrightness(target);
  }
}

void updateVoiceTapBanner() {
  if (!voiceTapBannerVisible ||
      static_cast<int32_t>(millis() - voiceTapBannerUntilMs) < 0) {
    return;
  }
  voiceTapBannerVisible = false;
  drawScreen();
}

void beginVoiceChatClick() {
  if (voiceClickReleasePending) {
    codex.sendKey(kVoiceChatCommandKey, 0);
  }
  codex.sendKey(kVoiceChatCommandKey, 1);
  voiceClickReleasePending = true;
  voiceClickReleaseAtMs = millis() + kVoiceClickPulseMs;
}

void updateVoiceChatClick() {
  if (!voiceClickReleasePending ||
      static_cast<int32_t>(millis() - voiceClickReleaseAtMs) < 0) {
    return;
  }
  codex.sendKey(kVoiceChatCommandKey, 0);
  voiceClickReleasePending = false;
}

dashboard::ThreadVisual threadVisual(const ThreadLight& light) {
  dashboard::ThreadVisual visual;
  visual.color = light.color;
  visual.brightness = light.brightness;
  // Current Codex Desktop marks the focused slot with s=0.4 while e remains
  // "off". Keep accepting the older explicit breath effect as a focus signal.
  visual.focused = light.effect == "breath" || light.speed > 0.01f;
  return visual;
}

dashboard::AgentStatus classifyAgent(const ThreadLight& light) {
  return dashboard::classify(threadVisual(light));
}

uint32_t quotaResetRemaining() {
  if (!state.quota.available) return 0;
  const uint32_t elapsed = (millis() - state.quota.receivedAtMs) / 1000;
  return elapsed >= state.quota.resetInSeconds ? 0 : state.quota.resetInSeconds - elapsed;
}

connection_health::Result connectionHealth() {
  connection_health::Input input;
  input.bleConnected = state.connected;
  input.hostRpcObserved = state.hostRpcObserved;
  input.lastHostRpcAtMs = state.lastHostRpcAtMs;
  input.quotaWaitingSinceMs = quotaWaitingSinceMs;
  input.quotaAvailable = state.quota.available;
  input.quotaReceivedAtMs = state.quota.receivedAtMs;
  return connection_health::evaluate(input, millis(), kHostRpcLiveForMs,
                                     kQuotaStaleAfterMs);
}

dashboard::LinkHealth dashboardLinkHealth(connection_health::Link link) {
  switch (link) {
    case connection_health::Link::CodexLive:
      return dashboard::LinkHealth::CodexLive;
    case connection_health::Link::BleOnly:
      return dashboard::LinkHealth::BleOnly;
    case connection_health::Link::Offline:
      return dashboard::LinkHealth::Offline;
  }
  return dashboard::LinkHealth::Offline;
}

dashboard::State dashboardState() {
  dashboard::State ui;
  const connection_health::Result health = connectionHealth();
  ui.linkHealth = dashboardLinkHealth(health.link);
  ui.batteryPercent = batteryPercent;
  ui.charging = charging;
  ui.docked = docked;
  ui.quotaAvailable = state.quota.available;
  ui.quotaStale = health.quota == connection_health::Quota::Stale;
  ui.remainingPercent = state.quota.remainingPercent;
  ui.resetInSeconds = quotaResetRemaining();
  ui.leftPressed = leftPressed;
  ui.rightPressed = rightPressed || voiceTapBannerVisible;
  ui.sendPressed = touchSendPressed;
  ui.activeTouchAgent = touchAgentPressed ? activeTouchAgent : -1;
  ui.swipeDirection = static_cast<int>(activeSwipe);
  ui.completedAgent =
      completionBanner.visible(millis()) ? completionBanner.agent : -1;
  ui.powerOverlay = powerOverlay;
  if (touchPowerHoldConsumed) {
    const uint32_t heldMs = millis() - touchSendStartedAtMs;
    ui.powerHoldProgress = std::max(
        0.0f, std::min(1.0f,
            static_cast<float>(heldMs - kPowerHoldPromptMs) /
                static_cast<float>(kTravelPowerOffMs - kPowerHoldPromptMs)));
  }
  for (int i = 0; i < 6; ++i) ui.threads[i] = threadVisual(state.threads[i]);
  return ui;
}

void drawScreen() {
  if (deskSleeping) return;
  const dashboard::State ui = dashboardState();
  dashboard::render(canvas, ui);
  canvas.pushSprite(0, 0);
  lastDrawMs = millis();
  renderedHealthValid = true;
  renderedLinkHealth = ui.linkHealth;
  renderedQuotaStale = ui.quotaStale;
  renderedBatteryPercent = ui.batteryPercent;
  renderedCharging = ui.charging;
  renderedDocked = ui.docked;
}

int8_t agentAtPoint(int x, int y) {
  return static_cast<int8_t>(dashboard::agentAtPoint(x, y));
}

void beginTouchAgent(int8_t agent) {
  if (agent < 0 || agent >= 6) return;
  touchAgentPressed = true;
  activeTouchAgent = agent;
  drawScreen();
}

void commitTouchAgent() {
  if (!touchAgentPressed || activeTouchAgent < 0) return;
  const int8_t agent = activeTouchAgent;
  touchLockoutUntilMs = millis() + kTouchCommitLockoutMs;
  lastCommitSpotX = touchStartX;
  lastCommitSpotY = touchStartY;
  startHaptic(kTouchHapticIntensity, kTouchHapticDurationMs);
  codex.sendKey(kAgentKeys[agent], 1, agent);
  delay(kTouchKeyPulseMs);
  codex.sendKey(kAgentKeys[agent], 0, agent);
  // Stop here for a deterministic pulse; leaving it to the loop-end check
  // stretched the buzz by however long the next full redraw took.
  stopHaptic();
  touchAgentPressed = false;
  activeTouchAgent = -1;
  drawScreen();
}

void beginTouchSend() {
  if (touchSendPressed) return;
  touchSendPressed = true;
  touchSendStartedAtMs = millis();
  touchPowerHoldConsumed = false;
  drawScreen();
}

void commitTouchSend() {
  if (!touchSendPressed) return;
  touchLockoutUntilMs = millis() + kTouchCommitLockoutMs;
  lastCommitSpotX = touchStartX;
  lastCommitSpotY = touchStartY;
  startHaptic(kTouchHapticIntensity, kTouchHapticDurationMs);
  codex.sendKey(kSendKey, 1);
  delay(kTouchKeyPulseMs);
  codex.sendKey(kSendKey, 0);
  stopHaptic();
  touchSendPressed = false;
  drawScreen();
}

void clearTouchCandidate() {
  touchAgentPressed = false;
  touchSendPressed = false;
  activeTouchAgent = -1;
}

void beginTouchGesture(int x, int y) {
  // CODEX_STOPWATCH_THIRD_PARTY: the CST9217 can fire several press/release
  // cycles for one physical tap. During the post-commit lockout, swallow only
  // the trailing cycles that land on the SAME SPOT as the commit; a press at a
  // different spot (another agent, the center Send dial) is a real new tap and
  // is allowed through immediately. A swallowed press extends the lockout so a
  // sustained stream of slow phantom cycles cannot leak a key between windows.
  if (static_cast<int32_t>(millis() - touchLockoutUntilMs) < 0) {
    const bool sameSpot =
        lastCommitSpotX >= 0 &&
        std::abs(x - lastCommitSpotX) <= kPhantomSameSpotPx &&
        std::abs(y - lastCommitSpotY) <= kPhantomSameSpotPx;
    if (sameSpot) {
      touchLockoutUntilMs = millis() + kTouchCommitLockoutMs;
      Serial.printf("DBG touch locked out x=%d y=%d\n", x, y);
      return;
    }
    // Different spot: a genuine new tap. Lift the lockout so the normal
    // gesture path runs for this press.
    touchLockoutUntilMs = 0;
    Serial.printf("DBG touch new spot x=%d y=%d\n", x, y);
  }
  touchTracking = true;
  touchStartX = static_cast<int16_t>(x);
  touchStartY = static_cast<int16_t>(y);
  touchBeganAtMs = millis();  // for phantom-tap suppression on release
  activeSwipe = touch_gesture::Direction::None;
  touchSideLeftActive = false;
  touchSideRightActive = false;

  const int8_t agent = agentAtPoint(x, y);
  Serial.printf("TOUCH begin x=%d y=%d agent=%d send=%d\n", x, y, agent,
                dashboard::sendAtPoint(x, y) ? 1 : 0);
  if (agent >= 0) {
    beginTouchAgent(agent);
  } else if (dashboard::sendAtPoint(x, y)) {
    beginTouchSend();
  }
}

void updateTouchGesture(int x, int y) {
  if (!touchTracking || touchPowerHoldConsumed ||
      touchSideLeftActive || touchSideRightActive ||
      activeSwipe != touch_gesture::Direction::None) {
    return;
  }
  const touch_gesture::Direction direction = touch_gesture::classifySwipe(
      x - touchStartX, y - touchStartY, kSwipeThresholdPx);
  Serial.printf("DBG swipe dx=%d dy=%d dir=%d\n", x - touchStartX,
                y - touchStartY, static_cast<int>(direction));
  if (direction == touch_gesture::Direction::None) return;

  clearTouchCandidate();
  activeSwipe = direction;
  const float angle = touch_gesture::normalizedAngle(direction);
  codex.sendJoystick(angle, 1.0f);
  startHaptic(kSwipeHapticIntensity, kSwipeHapticDurationMs);
  Serial.printf("SWIPE direction=%s angle=%.2f action=press\n",
                touch_gesture::name(direction), angle);
  drawScreen();
}

void finishTouchGesture() {
  if (!touchTracking) return;
  touchTracking = false;

  if (touchPowerHoldConsumed) {
    touchPowerHoldConsumed = false;
    powerOverlay = dashboard::PowerOverlay::None;
    clearTouchCandidate();
    drawScreen();
    return;
  }

  // CODEX_STOPWATCH_THIRD_PARTY: release emulated side keys on finger lift.
  if (touchSideLeftActive) {
    touchSideLeftActive = false;
    Serial.printf("SIDE left key=%s action=release\n", kLeftMicSwitch);
    releaseMicSwitch(kLeftMicSwitch, leftPressed);
    clearTouchCandidate();
    drawScreen();
    return;
  }
  if (touchSideRightActive) {
    touchSideRightActive = false;
    rightPressed = false;
    voiceTapBannerVisible = false;
    clearTouchCandidate();
    drawScreen();
    return;
  }

  if (activeSwipe != touch_gesture::Direction::None) {
    const touch_gesture::Direction direction = activeSwipe;
    const float angle = touch_gesture::normalizedAngle(direction);
    codex.sendJoystick(angle, 0.0f);
    Serial.printf("SWIPE direction=%s angle=%.2f action=release\n",
                  touch_gesture::name(direction), angle);
    activeSwipe = touch_gesture::Direction::None;
    clearTouchCandidate();
    drawScreen();
    return;
  }

  // CODEX_STOPWATCH_THIRD_PARTY: suppress phantom taps (CST9217 noise when
  // the watch lies on a desk): only commit Agent/Send if the finger was down
  // at least kMinTapHoldMs, otherwise clear the candidate.
  const bool heldLongEnough = millis() - touchBeganAtMs >= kMinTapHoldMs;
  const bool noiseSuppressed = suppressNoiseBurst();
  if (touchAgentPressed) {
    if (heldLongEnough && !noiseSuppressed) {
      commitTouchAgent();
    } else {
      clearTouchCandidate();
    }
  } else if (touchSendPressed) {
    if (heldLongEnough && !noiseSuppressed) {
      commitTouchSend();
    } else {
      clearTouchCandidate();
    }
  } else {
    clearTouchCandidate();
  }
}

void updateTouchPowerHold() {
  if (!touchTracking || activeSwipe != touch_gesture::Direction::None) return;
  if (!touchSendPressed && !touchPowerHoldConsumed) return;

  const uint32_t now = millis();
  const uint32_t heldMs = now - touchSendStartedAtMs;
  if (!touchPowerHoldConsumed && heldMs >= kPowerHoldPromptMs) {
    touchPowerHoldConsumed = true;
    touchSendPressed = false;  // releasing now must not emit Send
    powerOverlay = dashboard::PowerOverlay::HoldToPowerOff;
    lastPowerOverlayDrawMs = 0;
    startHaptic(kButtonHapticIntensity, kButtonHapticDurationMs);
  }
  if (!touchPowerHoldConsumed) return;
  if (heldMs >= kTravelPowerOffMs) enterTravelPowerOff();
  if (lastPowerOverlayDrawMs == 0 || now - lastPowerOverlayDrawMs >= 100) {
    lastPowerOverlayDrawMs = now;
    drawScreen();
  }
}

// CODEX_STOPWATCH_THIRD_PARTY: emulate the C152 side keys with long-presses
// on the left/right edge bands of the 466x466 touchscreen.
void updateTouchSideHold() {
  // CODEX_STOPWATCH_THIRD_PARTY: side-band emulation is DISABLED. The 1.75C
  // touchscreen reports phantom touches along the left edge when the watch is
  // placed on a desk (CST9217 noise), which held the Mic key down and left
  // ChatGPT stuck in "listening". The physical BOOT (Push to talk) and PWR
  // (voice chat toggle) buttons replace the bands.
  (void)touchTracking;
  (void)touchPowerHoldConsumed;
  (void)activeSwipe;
  (void)touchSideLeftActive;
  (void)touchSideRightActive;
  return;
}

void startCompletionChime(int8_t agent) {
  noteActivity();  // a finished agent is worth lighting the screen for
  startHaptic(kButtonHapticIntensity, kButtonHapticDurationMs);
  completionBanner.show(agent, millis(), kCompletionBannerMs);
#if defined(CODEX_STOPWATCH_USB_MIC)
  const auto chimeRequest = stopwatch_usb_mic::requestCompletionChime();
  Serial.printf("Agent %d completed; local_chime_request=%u\n", agent + 1,
                static_cast<unsigned>(chimeRequest));
#else
  M5.Speaker.playRaw(completionChimePcm.data(), completionChimePcm.size(),
                     kCompletionChimeSampleRate, false, 1, -1, true);
  Serial.printf("Agent %d completed; soft chime played\n", agent + 1);
#endif
}

bool expireCompletionBanner(uint32_t now) {
  return completionBanner.expire(now);
}

void detectAgentTransitions(const CodexMicroState& latest) {
  for (int i = 0; i < 6; ++i) {
    const dashboard::AgentStatus next = classifyAgent(latest.threads[i]);
    if (agentStatusObserved[i] &&
        previousAgentStatuses[i] != dashboard::AgentStatus::Complete &&
        next == dashboard::AgentStatus::Complete) {
      startCompletionChime(static_cast<int8_t>(i));
    }
    // Requires-input also wakes the screen; routine status refreshes must not,
    // or host polling would keep the panel lit around the clock.
    if (agentStatusObserved[i] && previousAgentStatuses[i] != next &&
        next == dashboard::AgentStatus::RequiresInput) {
      noteActivity();
    }
    previousAgentStatuses[i] = next;
    agentStatusObserved[i] = true;
  }
}

void observeDockState(bool candidate) {
  if (dockStateInitialized && candidate == docked) {
    pendingDockSamples = 0;
    return;
  }
  if (pendingDockSamples == 0 || pendingDockState != candidate) {
    pendingDockState = candidate;
    pendingDockSamples = 1;
    return;
  }
  if (++pendingDockSamples < 2) return;

  const bool changed = !dockStateInitialized || docked != candidate;
  docked = candidate;
  dockStateInitialized = true;
  pendingDockSamples = 0;
  if (!changed) return;

  Serial.printf("POWER mode=%s\n", docked ? "dock" : "battery");
  if (docked) {
    // Plugging into a dock is intentional activity and should reveal the
    // charging state even if the panel had already entered desk sleep.
    lastActivityMs = millis();
    if (deskSleeping) wakeDeskSleep();
  }
}

void updatePowerTelemetry(bool force = false) {
  const uint32_t now = millis();
  bool batteryChanged = false;

  if (force || lastBatteryMs == 0 ||
      now - lastBatteryMs >= kBatteryTelemetryIntervalMs) {
    lastBatteryMs = now;
    // CODEX_STOPWATCH_THIRD_PARTY: Waveshare 1.75C has no M5PM1; read the
    // AXP2101 instead. Falls back to -1 (unknown) exactly like PM1 failure.
    const int level = axpReady ? axp::batteryPercent()
                               : M5.Power.getBatteryLevel();
    const int8_t nextLevel =
        level < 0 ? -1 : static_cast<int8_t>(constrain(level, 0, 100));
    batteryChanged = batteryPercent != nextLevel;
    batteryPercent = nextLevel;
  }

  if (force || lastPowerTelemetryMs == 0 ||
      now - lastPowerTelemetryMs >= kPowerTelemetryIntervalMs) {
    lastPowerTelemetryMs = now;
    // CODEX_STOPWATCH_THIRD_PARTY: same swap for charging + VBUS.
    const bool nextCharging =
        axpReady ? axp::isCharging()
                 : M5.Power.isCharging() == m5::Power_Class::is_charging;
    batteryChanged = batteryChanged || charging != nextCharging;
    charging = nextCharging;

    const int vinMv = axpReady ? axp::vbusMv() : M5.Power.getVBUSVoltage();
    // Hysteresis prevents a marginal cable from bouncing power policy. VIN,
    // not charge state, defines Dock Mode because a full battery may not be
    // actively charging while it is still externally powered.
    const bool candidateDock = dockStateInitialized && docked
                                   ? vinMv >= 3500
                                   : vinMv >= 4000;
    observeDockState(candidateDock);
  }

  if (batteryChanged && batteryPercent >= 0) {
    codex.setBattery(static_cast<uint8_t>(batteryPercent), charging);
  }
}

void releaseControlsForPowerOff() {
  if (leftPressed) {
    codex.sendKey(kLeftMicSwitch, 0);
    leftPressed = false;
  }
  if (voiceClickReleasePending) {
    codex.sendKey(kVoiceChatCommandKey, 0);
    voiceClickReleasePending = false;
  }
  // CODEX_STOPWATCH_THIRD_PARTY: clear emulated side-key state.
  touchSideLeftActive = false;
  touchSideRightActive = false;
  // CODEX_STOPWATCH_THIRD_PARTY: release a held BOOT (PTT) button.
  if (bootButtonPressed) {
    codex.sendKey(kLeftMicSwitch, 0);
    bootButtonPressed = false;
  }
  if (activeSwipe != touch_gesture::Direction::None) {
    codex.sendJoystick(touch_gesture::normalizedAngle(activeSwipe), 0.0f);
    activeSwipe = touch_gesture::Direction::None;
  }
  rightPressed = false;
  voiceTapBannerVisible = false;
  touchTracking = false;
  clearTouchCandidate();
  codex.poll();
  delay(80);
  codex.poll();
}

[[noreturn]] void enterTravelPowerOff() {
  touchPowerHoldConsumed = true;
  powerOverlay = dashboard::PowerOverlay::PoweringOff;
  drawScreen();
  startHaptic(kButtonHapticIntensity, 60);
  delay(70);
  stopHaptic();
  delay(480);  // leave enough time to read the offline warning

  releaseControlsForPowerOff();
#if !defined(CODEX_STOPWATCH_USB_MIC)
  M5.Speaker.stop();
  M5.Speaker.end();
  // CODEX_STOPWATCH_THIRD_PARTY: cut the Waveshare PA GPIO.
  if (!powerManagerReady) board_audio::setSpeakerAmp(false);
#endif
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  M5.Display.waitDisplay();
  Serial.println("POWER travel_off wake=power_button_or_vin");
  Serial.flush();

  if (powerManagerReady) {
    const m5pm1_err_t result = powerManager.shutdown();
    Serial.printf("POWER PM1 shutdown returned=%d\n", static_cast<int>(result));
    Serial.flush();
  }
  // CODEX_STOPWATCH_THIRD_PARTY: on Waveshare 1.75C use the AXP2101 soft
  // power-off instead of PM1. A successful shutdown removes power; if it does
  // not (e.g. PEK not wired), the esp_restart() fallback below still applies.
  if (!powerManagerReady && axpReady) {
    axp::powerOff();
    Serial.println("POWER AXP2101 soft power-off sent");
    Serial.flush();
  }
  // A successful PM1 shutdown removes power and never reaches here. Retry the
  // PM1 command through M5Unified, but never fall into an unconfigured ESP
  // deep sleep: the PM1 button and VIN would not necessarily wake that state.
  const bool fallbackSent = M5.Power.M5pm1.powerOff();
  Serial.printf("POWER PM1 fallback sent=%d\n", fallbackSent ? 1 : 0);
  Serial.flush();
  delay(500);
  // Both PM1 commands failed if execution is still alive. Reboot to a usable
  // dashboard rather than leaving the device permanently black.
  esp_restart();
  while (true) delay(1000);
}

void beginPowerButtonPress() {
  powerButtonPressed = true;
  powerButtonWokeDisplay = deskSleeping;
  lastActivityMs = millis();
  if (deskSleeping) {
    wakeDeskSleep();
    startHaptic(kButtonHapticIntensity, kWakeHapticDurationMs);
  }
}

void finishPowerButtonPress() {
  if (!powerButtonPressed) return;
  powerButtonPressed = false;
  const power_button_gesture::Event event =
      powerButtonGesture.release(millis());
  if (event == power_button_gesture::Event::DoubleClick) {
    pendingPowerClickWokeDisplay = false;
    enterTravelPowerOff();
  }
  pendingPowerClickWokeDisplay = powerButtonWokeDisplay;
  powerButtonWokeDisplay = false;
}

void updatePowerButton() {
  static uint32_t lastPollMs = 0;
  const uint32_t now = millis();
  if (powerButtonPollingReady) {
    const power_button_gesture::Event delayed = powerButtonGesture.poll(now);
    if (delayed == power_button_gesture::Event::SingleClick) {
      if (!pendingPowerClickWokeDisplay) enterDeskSleep();
      pendingPowerClickWokeDisplay = false;
    }
    if (lastPollMs != 0 && now - lastPollMs < 25) return;
    lastPollMs = now;
    bool pressed = false;
    if (powerManager.btnGetState(&pressed) != M5PM1_OK) return;
    if (pressed && !powerButtonPressed) beginPowerButtonPress();
    if (!pressed && powerButtonPressed) finishPowerButtonPress();
    return;
  }

  // The full PM1 path should be available on C152. Keep a degraded fallback
  // so the red key still has deterministic behavior if direct PM1 init fails.
  if (M5.BtnPWR.wasClicked()) {
    if (deskSleeping) {
      lastActivityMs = now;
      wakeDeskSleep();
    } else {
      enterDeskSleep();
    }
  }
}

// CODEX_STOPWATCH_THIRD_PARTY: Waveshare 1.75C board buttons.
//   BOOT (GPIO0, active low): hold = Push to talk (ACT10), like C152 left key.
//   PWR  (AXP2101 PEK): short press = toggle voice chat (ACT09), like C152
//        right key. Long-press PEK is left to AXP2101 defaults (power off).
void updateBoardButtons() {
  const uint32_t now = millis();

  // --- BOOT button as Push to talk (debounced, level-triggered) ---
  const bool bootRaw = digitalRead(kBootButtonPin) == LOW;
  if (bootRaw != bootButtonPressed) {
    if (now - bootButtonChangedAtMs >= kBootButtonDebounceMs) {
      bootButtonPressed = bootRaw;
      bootButtonChangedAtMs = now;
      if (bootButtonPressed) {
        noteActivity();  // acts even from a dark screen
        Serial.printf("BUTTON boot key=%s action=press\n", kLeftMicSwitch);
        pressMicSwitch(kLeftMicSwitch, leftPressed);
      } else {
        Serial.printf("BUTTON boot key=%s action=release\n", kLeftMicSwitch);
        releaseMicSwitch(kLeftMicSwitch, leftPressed);
      }
    }
  } else {
    bootButtonChangedAtMs = now;
  }

  // --- PWR button (AXP2101 PEK) as voice-chat toggle ---
  if (axpReady) {
    const uint8_t pek = axp::pekPress();  // 0=none 1=long 2=short (auto-clear)
    if (pek == 2) {  // short press
      noteActivity();
      Serial.printf("BUTTON pwr key=%s action=click\n", kVoiceChatCommandKey);
      beginVoiceChatClick();
      drawScreen();
    } else if (pek == 1) {  // long press -> travel power off
      Serial.println("BUTTON pwr long-press -> travel off");
      enterTravelPowerOff();
    }
  }
}

void pressMicSwitch(const char* key, bool& pressed) {
  if (pressed) return;
  pressed = true;
  startHaptic(kButtonHapticIntensity, kButtonHapticDurationMs);
  codex.sendKey(key, 1);
  drawScreen();
}

void releaseMicSwitch(const char* key, bool& pressed) {
  if (!pressed) return;
  pressed = false;
  codex.sendKey(key, 0);
  drawScreen();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Codex Micro StopWatch port boot");

  auto config = M5.config();
  config.clear_display = true;
#if defined(CODEX_STOPWATCH_USB_MIC) && defined(CODEX_STOPWATCH_WAVESHARE_MIC)
  // CODEX_STOPWATCH_THIRD_PARTY: Waveshare USB-mic build records via the
  // onboard ES7210 on the shared I2S port (UsbMic.cpp), not M5Unified's C152
  // M5.Mic profile, so keep M5Unified from touching the mic. The ES8311
  // speaker is configured manually through BoardAudio.h.
  config.internal_spk = false;
  config.internal_mic = false;
#elif defined(CODEX_STOPWATCH_USB_MIC)
  config.internal_spk = false;
  config.internal_mic = true;
#else
  // CODEX_STOPWATCH_THIRD_PARTY: Waveshare 1.75C shares GPIO15 between the
  // touch SDA and the C152 I2S WS pin, so M5Unified must not auto-configure
  // the speaker; BoardAudio.h sets the Waveshare pins manually.
  config.internal_spk = false;
#endif
  M5.begin(config);
  // CODEX_STOPWATCH_THIRD_PARTY: read the CST9217 touch resolution register
  // (0xD1F8) so we can verify the raw coordinate range against the 466x466
  // framebuffer. Official Waveshare driver does this; M5GFX's Touch_CST226
  // does not, which can leave coordinates at half scale. The register address
  // must be byte-swapped like M5GFX's _read_reg16 does.
  {
    // Follow the official driver: enter command mode (write 0xD1, 0x01) first,
    // then read the resolution register 0xD1F8 as [0xD1, 0xF8] + 4 bytes.
    const uint8_t cmd[2] = {0xD1, 0x01};
    lgfx::i2c::transactionWrite(I2C_NUM_1, 0x5A, cmd, 2, 400000);
    delay(10);
    uint8_t reg[2] = {0xD1, 0xF8};
    uint8_t res[4] = {0};
    const bool resOk =
        lgfx::i2c::transactionWriteRead(I2C_NUM_1, 0x5A, reg, 2, res, 4,
                                        400000)
            .has_value();
    if (resOk) {
      const uint16_t resX = static_cast<uint16_t>((res[1] << 8) | res[0]);
      const uint16_t resY = static_cast<uint16_t>((res[3] << 8) | res[2]);
      Serial.printf("DBG CST9217 res x=%u y=%u raw=%02X %02X %02X %02X\n",
                    resX, resY, res[0], res[1], res[2], res[3]);
    } else {
      Serial.println("DBG CST9217 res read failed");
    }
  }
  quotaWaitingSinceMs = millis();
  M5.Display.setRotation(0);
  M5.Display.setBrightness(kActiveBrightness);
  M5.Display.setTextWrap(false);
  const m5pm1_err_t pm1Status = powerManager.begin(&M5.In_I2C);
  powerManagerReady = pm1Status == M5PM1_OK;
  if (powerManagerReady) {
    uint8_t wakeSource = 0;
    if (powerManager.getWakeSource(&wakeSource, M5PM1_CLEAN_ONCE) == M5PM1_OK) {
      Serial.printf("POWER wake_source=0x%02X\n", wakeSource);
    }
    // Travel Mode has no automatic timeout in this release. Clear a timer
    // left behind by factory/demo firmware so shutdown stays deterministic.
    const m5pm1_err_t timerStatus = powerManager.timerClear();
    if (timerStatus != M5PM1_OK) {
      Serial.printf("POWER stale timer clear failed=%d\n",
                    static_cast<int>(timerStatus));
    }
    const m5pm1_err_t resetStatus = powerManager.setSingleResetDisable(true);
    // Replace PM1's immediate hardware double-off with the same clean Travel
    // path used by the center hold. Long-hold Download remains enabled.
    const m5pm1_err_t doubleOffStatus =
        powerManager.setDoubleOffDisable(true);
    bool doubleOffDisabled = false;
    const m5pm1_err_t doubleOffReadStatus =
        powerManager.getDoubleOffDisable(&doubleOffDisabled);
    powerButtonPollingReady =
        resetStatus == M5PM1_OK && doubleOffStatus == M5PM1_OK &&
        doubleOffReadStatus == M5PM1_OK && doubleOffDisabled;
    if (!powerButtonPollingReady) {
      // If the software detector cannot be trusted, restore the documented
      // PM1 hardware double-click shutdown instead of leaving no red-button
      // power-off path. The six-second center hold remains available too.
      const m5pm1_err_t restoreDoubleOffStatus =
          powerManager.setDoubleOffDisable(false);
      Serial.printf(
          "POWER red-button config failed single=%d double=%d read=%d value=%d "
          "restore=%d\n",
          static_cast<int>(resetStatus), static_cast<int>(doubleOffStatus),
          static_cast<int>(doubleOffReadStatus), doubleOffDisabled ? 1 : 0,
          static_cast<int>(restoreDoubleOffStatus));
    } else {
      Serial.println("POWER red single=desk double=travel long=download");
    }
  } else {
    Serial.printf("POWER PM1 direct init failed=%d; using M5Unified fallback\n",
                  static_cast<int>(pm1Status));
    // CODEX_STOPWATCH_THIRD_PARTY: Waveshare 1.75C carries an AXP2101 on the
    // touch I2C bus instead of M5PM1. Try it for battery/charging/VBUS.
    axpReady = axp::init();
    Serial.printf("POWER AXP2101 ready=%d\n", axpReady ? 1 : 0);
    if (axpReady) {
      Serial.printf("POWER AXP vbat=%dmV vbus=%dmV soc=%d\n",
                    axp::vbatMv(), axp::vbusMv(), axp::soc());
    }
  }
  // CODEX_STOPWATCH_THIRD_PARTY: Waveshare BOOT button (GPIO0, active low,
  // pulled up) as Push to talk. Keep download mode working: esptool holds
  // GPIO0 low during reset, which this poll cannot interfere with.
  pinMode(kBootButtonPin, INPUT_PULLUP);
  bootButtonChangedAtMs = millis();
#if !defined(CODEX_STOPWATCH_USB_MIC)
  // CODEX_STOPWATCH_THIRD_PARTY: Waveshare 1.75C ES8311 must be configured
  // manually (M5Unified's C152 profile would clash with the touch bus).
  bool audioReady = board_audio::begin();
  Serial.printf("AUDIO ES8311 ready=%d\n", audioReady ? 1 : 0);
  M5.Speaker.setVolume(kCompletionChimeVolume);
  buildCompletionChime();
#endif

  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (canvas.createSprite(M5.Display.width(), M5.Display.height()) == nullptr) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("Canvas allocation failed", M5.Display.width() / 2,
                          M5.Display.height() / 2);
    Serial.println("Canvas allocation failed");
    while (true) delay(1000);
  }
  canvas.setTextWrap(false);

#if defined(CODEX_STOPWATCH_USB_MIC)
  if (!stopwatch_usb_mic::begin()) {
    Serial.println("USB_MIC_FAILED");
  }
#endif

  codex.begin();
  state = codex.snapshot();
  lastActivityMs = millis();
  updatePowerTelemetry(true);
  drawScreen();
  Serial.println("CODEX_MICRO_STOPWATCH_READY");
}

void loop() {
  M5.update();
  codex.poll();
  updatePowerButton();
  // CODEX_STOPWATCH_THIRD_PARTY: Waveshare board buttons (BOOT PTT / PWR VC).
  updateBoardButtons();

  const auto touch = M5.Touch.getDetail();
  // CODEX_STOPWATCH_THIRD_PARTY: the CST9217 on the Waveshare 1.75C reports
  // ~0-232 coordinates that are also MIRRORED (top-left maps to (232,232),
  // bottom-right to (0,0)). Mirror and scale to the 466x466 framebuffer:
  // screen = (232 - raw) * 2. Without the mirror, tapping A5 (top-left)
  // landed on A2 (bottom-right, the diagonal opposite).
  const int touchX = (232 - touch.x) * 2;
  const int touchY = (232 - touch.y) * 2;
  if (touch.wasPressed()) {
    Serial.printf("DBG T press x=%d y=%d\n", touchX, touchY);
    if (noteActivity()) {
      beginTouchGesture(touchX, touchY);
    } else {
      startHaptic(kButtonHapticIntensity, kWakeHapticDurationMs);
    }
  }
  if (touchTracking && touch.isPressed()) {
    // DBG: log coordinate changes (throttled) to verify movement tracking.
    if (touchX != lastDbgTx || touchY != lastDbgTy) {
      Serial.printf("DBG T move x=%d y=%d\n", touchX, touchY);
      lastDbgTx = static_cast<int16_t>(touchX);
      lastDbgTy = static_cast<int16_t>(touchY);
    }
    updateTouchGesture(touchX, touchY);
    updateTouchSideHold();
    updateTouchPowerHold();
  }
  if (touch.wasReleased()) {
    Serial.printf("DBG T release x=%d y=%d\n", touchX, touchY);
    finishTouchGesture();
  }

  // Physical validation on the C152 enclosure: BtnA/GPIO2 is the left key and
  // BtnB/GPIO1 is the right key.
  if (M5.BtnA.wasPressed()) {
    noteActivity();  // physical keys act even from a dark screen
    Serial.printf("BUTTON physical=left key=%s action=press\n", kLeftMicSwitch);
    pressMicSwitch(kLeftMicSwitch, leftPressed);
  }
  if (M5.BtnA.wasReleased()) {
    Serial.printf("BUTTON physical=left key=%s action=release\n", kLeftMicSwitch);
    releaseMicSwitch(kLeftMicSwitch, leftPressed);
  }
  if (M5.BtnB.wasPressed()) {
    noteActivity();
    rightPressedAtMs = millis();
    rightPressed = true;
    voiceTapBannerVisible = true;
    voiceTapBannerUntilMs = millis() + kVoiceTapBannerMs;
    startHaptic(kButtonHapticIntensity, kButtonHapticDurationMs);
    drawScreen();
  }
  if (M5.BtnB.wasReleased()) {
    const uint32_t heldMs = millis() - rightPressedAtMs;
    rightPressed = false;
    beginVoiceChatClick();
    Serial.printf("BUTTON physical=right key=%s physical_hold=%lums click_pulse=%lums\n",
                  kVoiceChatCommandKey, static_cast<unsigned long>(heldMs),
                  static_cast<unsigned long>(kVoiceClickPulseMs));
    drawScreen();
  }

  CodexMicroState latest = codex.snapshot();
  const bool shouldRedraw = latest.dirty;
  if (shouldRedraw) {
    detectAgentTransitions(latest);
  }
  state = latest;
  updatePowerTelemetry();
  const bool completionBannerExpired = expireCompletionBanner(millis());

  const dashboard::State currentUi = dashboardState();
  const bool derivedStateChanged =
      !renderedHealthValid || currentUi.linkHealth != renderedLinkHealth ||
      currentUi.quotaStale != renderedQuotaStale ||
      currentUi.batteryPercent != renderedBatteryPercent ||
      currentUi.charging != renderedCharging || currentUi.docked != renderedDocked;
  if (!deskSleeping &&
      (shouldRedraw || derivedStateChanged || completionBannerExpired)) {
    drawScreen();
  }

  if (appliedBrightness > 0 && state.quota.available &&
      millis() - lastDrawMs > 30000) {
    drawScreen();
  }

  updateIdleDimming();
  updateHaptic();
  updateVoiceTapBanner();
  updateVoiceChatClick();
  delay(8);
}
