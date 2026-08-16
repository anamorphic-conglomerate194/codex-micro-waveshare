// SPDX-License-Identifier: MIT
// CODEX_STOPWATCH_THIRD_PARTY: ES8311 audio for Waveshare
// ESP32-S3-Touch-AMOLED-1.75C. The board has no M5PM1/M5IOE1 and its I2S
// wiring differs from the C152: MCLK=16, BCLK=9, WS(LRCK)=45, DOUT=8,
// amplifier enable PA=GPIO46 (active high). The ES8311 codec (0x18) shares
// the touch I2C bus (GPIO14=SCL, GPIO15=SDA, I2C_NUM_1), so we drive it
// through lgfx::i2c::* like the AXP2101 instead of installing a second I2C
// driver. main.cpp must call M5.begin() with config.internal_spk = false,
// because M5Unified's C152 StopWatch profile would put I2S WS on GPIO15,
// which is the touch SDA on this board.
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <lgfx/v1/platforms/common.hpp>

namespace board_audio {

// ES8311 registers (same sequence M5Unified uses on the C152).
static constexpr uint8_t kEs8311Address = 0x18;
static constexpr uint8_t kRegReset        = 0x00;
static constexpr uint8_t kRegClockSource  = 0x01;
static constexpr uint8_t kRegClockMult    = 0x02;
static constexpr uint8_t kRegAnalogPower  = 0x0D;
static constexpr uint8_t kRegDacPower     = 0x12;
static constexpr uint8_t kRegOutputEnable = 0x13;
static constexpr uint8_t kRegDacVolume    = 0x32;
static constexpr uint8_t kRegDacEq        = 0x37;

// Waveshare 1.75C I2S pins.
static constexpr int kMclkPin = 16;
static constexpr int kBclkPin = 9;
static constexpr int kWsPin   = 45;
static constexpr int kDataOutPin = 8;
static constexpr int kAmpEnablePin = 46;  // PA, active high

inline bool writeCodecReg(uint8_t reg, uint8_t val) {
  return lgfx::i2c::writeRegister8(I2C_NUM_1, kEs8311Address, reg, val, 0,
                                   100000)
      .has_value();
}

// Bring the ES8311 into a known DAC-output state (C152-compatible profile).
inline bool configureCodecSpeaker() {
  bool ok = writeCodecReg(kRegReset, 0x80);
  ok = writeCodecReg(kRegClockSource, 0xB5) && ok;
  ok = writeCodecReg(kRegClockMult, 0x18) && ok;
  ok = writeCodecReg(kRegAnalogPower, 0x01) && ok;
  ok = writeCodecReg(kRegDacPower, 0x00) && ok;
  ok = writeCodecReg(kRegOutputEnable, 0x10) && ok;
  ok = writeCodecReg(kRegDacVolume, 0xEF) && ok;
  ok = writeCodecReg(kRegDacEq, 0x08) && ok;
  return ok;
}

// Point M5Unified's Speaker at the Waveshare I2S pins (internal_spk=false
// means M5Unified never configured them from the C152 table).
inline void configureLocalSpeaker() {
  auto config = M5.Speaker.config();
  config.pin_mck = kMclkPin;
  config.pin_bck = kBclkPin;
  config.pin_ws = kWsPin;
  config.pin_data_out = kDataOutPin;
  config.i2s_port = I2S_NUM_0;
  config.magnification = 4;
  config.sample_rate = 44100;
  config.stereo = true;
  config.buzzer = false;
  config.use_dac = false;
  config.dac_zero_level = 0;
  M5.Speaker.config(config);
}

inline void setSpeakerAmp(bool enabled) {
  pinMode(kAmpEnablePin, OUTPUT);
  digitalWrite(kAmpEnablePin, enabled ? HIGH : LOW);
}

// Call once after M5.begin() (with internal_spk disabled). Returns true when
// the codec answered; a failure only disables the chime, not the dashboard.
inline bool begin() {
  configureLocalSpeaker();
  setSpeakerAmp(true);
  delay(10);
  const bool ok = configureCodecSpeaker();
  if (!ok) setSpeakerAmp(false);
  return ok;
}

inline void suspend() {
  if (M5.Speaker.isRunning()) M5.Speaker.end();
  setSpeakerAmp(false);
}

inline void resume() {
  configureLocalSpeaker();
  setSpeakerAmp(true);
  delay(10);
  M5.Speaker.begin();
  M5.Speaker.setVolume(160);
}

inline void shutdown() {
  M5.Speaker.stop();
  M5.Speaker.end();
  setSpeakerAmp(false);
}

}  // namespace board_audio

