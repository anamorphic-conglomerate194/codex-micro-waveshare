// SPDX-License-Identifier: MIT
// CODEX_STOPWATCH_THIRD_PARTY: AXP2101 power management for Waveshare
// ESP32-S3-Touch-AMOLED-1.75C. The board has no M5PM1/M5IOE1, but carries an
// AXP2101 PMIC on the same I2C bus as the touch controller (GPIO14=SCL,
// GPIO15=SDA, I2C_NUM_1), which M5GFX already initialized for the touch.
// We reuse that bus via lgfx::i2c::* instead of installing a second I2C
// driver (XPowersLib/TwoWire would conflict with the touch on 14/15).
#pragma once

#include <lgfx/v1/platforms/common.hpp>  // lgfx::i2c::readRegister8/writeRegister8

namespace axp {

static constexpr int kPort = I2C_NUM_1;
static constexpr uint8_t kAddr = 0x34;
static constexpr uint32_t kFreq = 400000;

// Register map (subset actually needed; do NOT touch LDO/power-rail regs).
static constexpr uint16_t kRegStatus    = 0x00;  // bit5=VBUS in
static constexpr uint16_t kRegCharge    = 0x01;  // bits[6:5]=charge status
static constexpr uint16_t kRegChipId    = 0x03;  // AXP2101 == 0x4A
static constexpr uint16_t kRegPowerOff  = 0x10;  // bit0=soft power off
static constexpr uint16_t kRegAdc       = 0x30;  // ADC enable
static constexpr uint16_t kRegVbat      = 0x34;  // 14-bit battery voltage
static constexpr uint16_t kRegVbus      = 0x38;  // 14-bit VBUS voltage
static constexpr uint16_t kRegSoc       = 0xA4;  // coulomb counter SOC 0-100
static constexpr uint16_t kRegPek       = 0x49;  // PEK press status bits[3:2]

inline uint8_t readReg(uint8_t reg) {
  auto v = lgfx::i2c::readRegister8(kPort, kAddr, reg, kFreq);
  return v.has_value() ? v.value() : 0xFF;  // 0xFF = failure sentinel
}

inline bool writeReg(uint8_t reg, uint8_t val) {
  return lgfx::i2c::writeRegister8(kPort, kAddr, reg, val, 0, kFreq).has_value();
}

// Initialize: verify chip ID, then enable the ADC so voltage reads work.
// The I2C bus itself is already up (touch), so lgfx::i2c::init is idempotent.
inline bool init() {
  lgfx::i2c::init(kPort, GPIO_NUM_15, GPIO_NUM_14);
  if (readReg(kRegStatus) == 0xFF) return false;   // bus probe
  if (readReg(kRegChipId) != 0x4A) return false;   // AXP2101 ID check
  writeReg(kRegAdc, 0b111111);                     // enable all ADC channels
  return true;
}

// 14-bit big-ish endian voltage registers; returns mV, -1 on failure.
inline int16_t voltageMv(uint16_t reg) {
  uint8_t hi = readReg(static_cast<uint8_t>(reg));
  if (hi == 0xFF) return -1;
  uint8_t lo = readReg(static_cast<uint8_t>(reg) + 1);
  if (lo == 0xFF) return -1;
  return static_cast<int16_t>(((hi & 0x3F) << 8) | lo);
}

inline int16_t vbatMv() { return voltageMv(kRegVbat); }
inline int16_t vbusMv() { return voltageMv(kRegVbus); }

inline bool isCharging() {
  return (readReg(kRegCharge) & 0b01100000) == 0b00100000;
}

inline bool isVbusIn() { return readReg(kRegStatus) & 0x20; }

inline int8_t soc() {
  uint8_t s = readReg(kRegSoc);
  return s <= 100 ? static_cast<int8_t>(s) : -1;
}

// SOC from the coulomb counter, falling back to a linear voltage estimate
// (single-cell LiPo ~3.3-4.15V) when the counter is uncalibrated (0/0xFF).
inline int8_t batteryPercent() {
  int8_t s = soc();
  if (s >= 0) return s;
  int16_t mv = vbatMv();
  if (mv < 0) return -1;
  int p = (mv - 3300) * 100 / (4150 - 3300);
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

inline void powerOff() {
  uint8_t v = readReg(kRegPowerOff);
  if (v != 0xFF) writeReg(kRegPowerOff, v | 0x01);
}

// PEK press: 0=none 1=long 2=short; read clears the flag.
inline uint8_t pekPress() {
  uint8_t v = readReg(kRegPek) & 0x0C;
  if (v) writeReg(kRegPek, v);
  return v >> 2;
}

}  // namespace axp

