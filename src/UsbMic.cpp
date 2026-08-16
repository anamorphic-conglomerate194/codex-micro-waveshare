// SPDX-License-Identifier: MIT

#include "UsbMic.h"

#if defined(CODEX_STOPWATCH_USB_MIC)

#include <Arduino.h>
#include <M5Unified.h>
#include <USB.h>
#include <USBAudioCard.h>
#include <array>
#include <atomic>
#include <cmath>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <tusb.h>
#if defined(CODEX_STOPWATCH_WAVESHARE_MIC)
#include <driver/i2s_std.h>
#include <lgfx/v1/platforms/common.hpp>
#include "Axp2101Power.h"
#endif

#ifndef CODEX_STOPWATCH_USB_MIC_SYNTHETIC
#define CODEX_STOPWATCH_USB_MIC_SYNTHETIC 0
#endif

#if !defined(CODEX_STOPWATCH_DISABLE_M5_MIC_DC_SERVO)
#error "USB mic build must gate M5Unified's fixed-rate DC servo"
#endif

// Arduino-ESP32 3.3.11 hard-codes "TinyUSB UAC1" as the AudioControl
// interface string. The usb-mic build wraps only that descriptor insertion so
// macOS presents a product-specific input name without carrying a patched
// framework checkout in this repository.
extern "C" uint8_t __real_tinyusb_add_string_descriptor(const char* value);
extern "C" uint8_t __wrap_tinyusb_add_string_descriptor(const char* value) {
  constexpr char kArduinoAudioInterfaceName[] = "TinyUSB UAC1";
  // tinyusb_add_string_descriptor() retains the pointer instead of copying the
  // text, so the replacement must outlive this wrapper call.
  static constexpr char kUsbMicName[] = "Codex StopWatch Mic";
  if (value != nullptr && strcmp(value, kArduinoAudioInterfaceName) == 0) {
    value = kUsbMicName;
  }
  return __real_tinyusb_add_string_descriptor(value);
}

namespace {

#if defined(CODEX_STOPWATCH_WAVESHARE_MIC)
// ---------------------------------------------------------------------------
// Waveshare 1.75C capture backend: onboard ES7210 dual digital mics over the
// shared I2S_NUM_0 (BCLK=9, WS=45, MCLK=16, DIN=10). ES7210 outputs STEREO
// (L=MIC1, R=MIC2) in non-TDM mode, so we read 16-bit stereo frames and mix
// down to the mono 48 kHz PCM the USB pipeline expects. The ES7210 and ES8311
// share the I2S port; the speaker (chime) must not run while capturing.
// ---------------------------------------------------------------------------
namespace ws_mic {

constexpr int kI2sPort = I2S_NUM_0;
constexpr int kPinBclk = 9;
constexpr int kPinWs = 45;
constexpr int kPinMclk = 16;
constexpr int kPinDin = 10;
constexpr uint8_t kEs7210Addr = 0x40;
constexpr uint32_t kEs7210Freq = 400000;

i2s_chan_handle_t rxHandle = nullptr;
std::atomic<bool> capturing{false};
std::atomic<uint32_t> recordSeq{0};

inline void writeReg(uint8_t reg, uint8_t val) {
  lgfx::i2c::writeRegister8(I2C_NUM_1, kEs7210Addr, reg, val, 0, kEs7210Freq);
}
inline uint8_t readReg(uint8_t reg) {
  auto v = lgfx::i2c::readRegister8(I2C_NUM_1, kEs7210Addr, reg, kEs7210Freq);
  return v.has_value() ? v.value() : 0xFF;
}

bool initEs7210() {
  // Software reset
  writeReg(0x00, 0xFF);
  delay(10);
  writeReg(0x00, 0x32);
  delay(10);
  // Clock off during config
  writeReg(0x01, 0x3F);
  // Power-up timing
  writeReg(0x09, 0x30);
  writeReg(0x0A, 0x30);
  // HPF for ADC1-4
  writeReg(0x23, 0x2A);
  writeReg(0x22, 0x0A);
  writeReg(0x21, 0x2A);
  writeReg(0x20, 0x0A);
  // Analog power pre-on
  writeReg(0x40, 0xC3);
  // MIC bias 2.87V
  writeReg(0x41, 0x70);
  writeReg(0x42, 0x70);
  // MIC gain: enable + 30dB
  for (uint8_t r = 0x43; r <= 0x46; ++r) writeReg(r, 0x1A);
  // PGA power
  for (uint8_t r = 0x47; r <= 0x4A; ++r) writeReg(r, 0x08);
  // Mic power (all off; select below)
  writeReg(0x4B, 0xFF);
  writeReg(0x4C, 0xFF);
  // I2S format: 16-bit, standard I2S, non-TDM (dual: ADC12 -> SDOUT1)
  writeReg(0x11, 0x60);
  writeReg(0x12, 0x00);
  // Start sequence (esp_codec_dev es7210_start)
  writeReg(0x01, 0x3F);
  writeReg(0x06, 0x00);  // DLL power
  writeReg(0x40, 0x43);  // analog power really on
  for (uint8_t r = 0x47; r <= 0x4A; ++r) writeReg(r, 0x08);
  writeReg(0x01, 0x3F & ~0x0B);  // enable MIC12 clocks + power
  writeReg(0x4B, 0x00);
  writeReg(0x40, 0x43);
  writeReg(0x00, 0x71);
  delay(50);
  writeReg(0x00, 0x41);
  delay(50);
  Serial.printf("ES7210 id0=0x%02X\n", readReg(0x00));
  return true;
}

bool initI2sRx() {
  i2s_chan_config_t chanCfg = {};
  chanCfg.id = (i2s_port_t)kI2sPort;
  chanCfg.role = I2S_ROLE_MASTER;
  chanCfg.dma_desc_num = 4;
  chanCfg.dma_frame_num = kDmaFrames;
  chanCfg.auto_clear = true;
  if (i2s_new_channel(&chanCfg, nullptr, &rxHandle) != ESP_OK) {
    Serial.println("ES7210 i2s_new_channel rx failed");
    return false;
  }
  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg.sample_rate_hz = kSampleRate;
  stdCfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  stdCfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
  stdCfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
  stdCfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
  stdCfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  stdCfg.slot_cfg.ws_width = 16;
  stdCfg.slot_cfg.ws_pol = false;
  stdCfg.slot_cfg.bit_shift = true;
  stdCfg.slot_cfg.msb_right = false;
  stdCfg.slot_cfg.big_endian = false;
  stdCfg.slot_cfg.bit_order_lsb = false;
  stdCfg.slot_cfg.left_align = true;
  stdCfg.gpio_cfg.mclk = (gpio_num_t)kPinMclk;
  stdCfg.gpio_cfg.bclk = (gpio_num_t)kPinBclk;
  stdCfg.gpio_cfg.ws = (gpio_num_t)kPinWs;
  stdCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.din = (gpio_num_t)kPinDin;
  if (i2s_channel_init_std_mode(rxHandle, &stdCfg) != ESP_OK) {
    Serial.println("ES7210 i2s_channel_init_std_mode failed");
    i2s_del_channel(rxHandle);
    rxHandle = nullptr;
    return false;
  }
  if (i2s_channel_enable(rxHandle) != ESP_OK) {
    Serial.println("ES7210 i2s_channel_enable failed");
    return false;
  }
  return true;
}

bool begin() {
  // Enable AXP2101 ALDO1 (3.3V) for the mics.
  axp::writeReg(0x92, 0x1C);  // ALDO1 voltage 3.3V
  delay(10);
  uint8_t ldo = axp::readReg(0x90);
  axp::writeReg(0x90, ldo | 0x01);  // enable ALDO1
  delay(10);
  if (!initEs7210()) return false;
  if (!initI2sRx()) return false;
  capturing.store(true, std::memory_order_release);
  return true;
}

bool record(int16_t* monoOut, size_t samples, uint32_t rate) {
  if (!capturing.load(std::memory_order_acquire) || rxHandle == nullptr) {
    return false;
  }
  // ES7210 outputs stereo frames; read into a scratch stereo buffer and mix.
  static std::array<int32_t, kDmaFrames * 2> stereo = {};
  constexpr size_t frameBytes = kDmaFrames * 2 * sizeof(int16_t);
  size_t bytesRead = 0;
  if (i2s_channel_read(rxHandle, stereo.data(), frameBytes, &bytesRead, 100) !=
      ESP_OK) {
    return false;
  }
  const size_t frames = bytesRead / (2 * sizeof(int16_t));
  for (size_t i = 0; i < frames && i < samples; ++i) {
    int32_t l = stereo[i * 2];
    int32_t r = stereo[i * 2 + 1];
    monoOut[i] = static_cast<int16_t>((l + r) >> 1);
  }
  if (frames < samples) {
    for (size_t i = frames; i < samples; ++i) monoOut[i] = 0;
  }
  recordSeq.fetch_add(1, std::memory_order_relaxed);
  return true;
}

uint32_t isRecording() { return capturing.load(std::memory_order_acquire) ? 2 : 0; }

void end() {
  capturing.store(false, std::memory_order_release);
  if (rxHandle != nullptr) {
    i2s_channel_disable(rxHandle);
    i2s_del_channel(rxHandle);
    rxHandle = nullptr;
  }
}

}  // namespace ws_mic

// Minimal M5.Mic-compatible proxy so the untouched C152 pipeline code below
// uses the ES7210 backend when CODEX_STOPWATCH_WAVESHARE_MIC is defined.
struct WaveshareMicProxy {
  struct Cfg {
    uint32_t sample_rate = 0;
    int input_channel = 0;
    uint32_t dma_buf_len = 0;
    uint32_t dma_buf_count = 0;
    uint32_t over_sampling = 0;
    uint32_t magnification = 0;
    uint32_t task_priority = 0;
  };
  static bool begin() { return ws_mic::begin(); }
  static void end() { ws_mic::end(); }
  static bool record(int16_t* buf, size_t n, uint32_t r) {
    return ws_mic::record(buf, n, r);
  }
  static uint32_t isRecording() { return ws_mic::isRecording(); }
  static Cfg config() { return Cfg{}; }
  static void config(const Cfg&) {}
};
#define M5_Mic WaveshareMicProxy
#endif  // CODEX_STOPWATCH_WAVESHARE_MIC



constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kBlockDurationMs = 10;
constexpr size_t kSamplesPerBlock =
    kSampleRate * kBlockDurationMs / 1000;
constexpr size_t kBlockBytes = kSamplesPerBlock * sizeof(int16_t);
constexpr UBaseType_t kQueuedBlocks = 2;
constexpr size_t kDmaFrames = 480;
constexpr uint32_t kTaskStackBytes = 4096;
constexpr UBaseType_t kUsbTaskPriority = 3;
constexpr UBaseType_t kCaptureTaskPriority = kUsbTaskPriority + 1;
constexpr size_t kStartupSilenceBytes = 256;
constexpr uint32_t kStartupPrefillWriteLimit = 8;
constexpr uint32_t kStartupTopUpPollMs = 1;
constexpr uint32_t kChimeIdleGuardMs = 200;

enum class ChimePhase : uint8_t {
  Idle,
  Arming,
  Guarding,
  Switching,
  Playing,
  Finishing,
};

#if !CODEX_STOPWATCH_USB_MIC_SYNTHETIC
// The StopWatch routes its analog MEMS microphone through an ES8311. M5Unified
// starts that codec with minimum analog PGA gain and maximum ADC volume. This
// profile trades 6 dB of analog PGA for 6 dB of digital gain, preserving the
// nominal level while giving speech peaks more analog headroom.
constexpr uint8_t kEs8311Address = 0x18;
constexpr uint8_t kEs8311AdcPgaRegister = 0x14;
constexpr uint8_t kEs8311AdcScaleRegister = 0x16;
constexpr uint8_t kEs8311AdcVolumeRegister = 0x17;
constexpr uint8_t kEs8311AdcHpfRegister = 0x1C;
constexpr uint8_t kEs8311DifferentialInputPga24Db = 0x18;
constexpr uint8_t kEs8311AdcScale24Db = 0x04;
constexpr uint8_t kEs8311AdcDigitalPlus6Db = 0xCB;
// Keep the codec's dynamic DC-cancellation/high-pass stage enabled. The
// USB-mic build separately disables M5Unified's fixed-rate software DC servo,
// which otherwise introduces an additional ~121 Hz corner at 48 kHz.
constexpr uint8_t kEs8311AdcDynamicHpf = 0x6A;
constexpr uint32_t kEs8311I2cFrequency = 100000;
constexpr int kEs8311I2cPort = 1;
constexpr int kEs8311SdaPin = 47;
constexpr int kEs8311SclPin = 48;

// A subdued, local-only completion sound. USBAudioCard remains UAC_SPK_NONE
// below; this volume applies only during the brief physical-speaker chime.
constexpr uint32_t kChimeSampleRate = 12000;
constexpr uint32_t kChimeDurationMs = 270;
constexpr size_t kChimeSamples =
    kChimeSampleRate * kChimeDurationMs / 1000;
constexpr float kPi = 3.14159265358979323846f;
constexpr uint8_t kChimeVolume = 160;
constexpr uint32_t kChimePlaybackTimeoutMs = 500;
// isPlaying() clears when the PCM source is exhausted, before M5Unified has
// necessarily drained its I2S DMA queue. Keep the PA alive for one bounded
// quiet tail; an arriving alt1 request still preempts it within one tick.
constexpr uint32_t kChimeDmaDrainMs = 48;

constexpr int kSpeakerMclkPin = 18;
constexpr int kSpeakerBclkPin = 17;
constexpr int kSpeakerWordSelectPin = 15;
constexpr int kSpeakerDataOutPin = 21;
constexpr uint8_t kAudioPowerIoePin = 2;  // physical M5IOE1 G3
constexpr uint8_t kSpeakerAmpIoePin = 9;  // physical M5IOE1 G10

constexpr uint8_t kEs8311ResetRegister = 0x00;
constexpr uint8_t kEs8311ClockSourceRegister = 0x01;
constexpr uint8_t kEs8311ClockMultiplierRegister = 0x02;
constexpr uint8_t kEs8311AnalogPowerRegister = 0x0D;
constexpr uint8_t kEs8311DacPowerRegister = 0x12;
constexpr uint8_t kEs8311OutputEnableRegister = 0x13;
constexpr uint8_t kEs8311DacVolumeRegister = 0x32;
constexpr uint8_t kEs8311DacEqualizerRegister = 0x37;
#endif

struct AudioBlock {
  uint32_t generation;
  int16_t samples[kSamplesPerBlock];
};

struct SilenceFillResult {
  uint32_t calls;
  uint64_t bytes;
};

// The official Arduino-ESP32 class owns all UAC1 descriptors and control
// callbacks. The explicit channel choices make this an input-only mono device.
USBAudioCard audioCard(kSampleRate, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_MONO);

int16_t captureBuffers[2][kSamplesPerBlock] = {};
uint32_t captureGenerations[2] = {};
bool captureStartedActive[2] = {};
AudioBlock enqueueBlock = {};
AudioBlock usbTransferBlock = {};
AudioBlock producerDropBlock = {};
AudioBlock transitionDropBlock = {};
const uint8_t silenceBlock[kStartupSilenceBytes] = {};
StaticQueue_t audioQueueControl = {};
uint8_t audioQueueStorage[kQueuedBlocks * sizeof(AudioBlock)] = {};
QueueHandle_t audioQueue = nullptr;
StaticSemaphore_t usbFifoMutexControl = {};
SemaphoreHandle_t usbFifoMutex = nullptr;
TaskHandle_t usbTask = nullptr;
TaskHandle_t captureTask = nullptr;
std::atomic<bool> usbMicActive{false};
std::atomic<bool> micInterfaceEnabled{false};
std::atomic<bool> usbLinkUsable{false};
std::atomic<bool> captureHardwareReady{false};
std::atomic<uint32_t> streamGeneration{0};
// Incremented for every observed alt1 request. Comparing generations instead
// of clearing a boolean latch preserves even a very short alt1 -> alt0 pulse
// that races request arming on the other core.
std::atomic<uint32_t> micInterfaceEnableGeneration{0};
// Zero means no startup fill is pending. Active generations begin at one; a
// compare/exchange prevents an old feeder iteration from clearing a newer one.
std::atomic<uint32_t> startupAwaitingGeneration{0};
std::atomic<bool> pipelineStarted{false};
std::atomic<ChimePhase> chimePhase{ChimePhase::Idle};
std::atomic<uint32_t> chimeAlt1Baseline{0};
std::atomic<uint32_t> chimeGuardUntilMs{0};
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
stopwatch_usb_mic::Stats pipelineStats = {};
uint32_t chimeSequence = 0;
stopwatch_usb_mic::ChimeResult latestChimeResult =
    stopwatch_usb_mic::ChimeResult::NeverRequested;

#if !CODEX_STOPWATCH_USB_MIC_SYNTHETIC
std::array<int16_t, kChimeSamples> completionChimePcm = {};
#endif

#if !CODEX_STOPWATCH_USB_MIC_SYNTHETIC
bool configureCodecSpeechGain() {
  m5gfx::i2c::i2c_temporary_switcher_t codecBus(
      kEs8311I2cPort, kEs8311SdaPin, kEs8311SclPin);

  // Preserve the nominal total gain while reducing analog PGA by 6 dB and
  // compensating digitally. M5Unified's magnification remains unity.
  const bool volumeWritten = M5.In_I2C.writeRegister8(
      kEs8311Address, kEs8311AdcVolumeRegister,
      kEs8311AdcDigitalPlus6Db, kEs8311I2cFrequency);
  const bool scaleWritten = M5.In_I2C.writeRegister8(
      kEs8311Address, kEs8311AdcScaleRegister,
      kEs8311AdcScale24Db, kEs8311I2cFrequency);
  const bool pgaWritten = M5.In_I2C.writeRegister8(
      kEs8311Address, kEs8311AdcPgaRegister,
      kEs8311DifferentialInputPga24Db, kEs8311I2cFrequency);
  const bool hpfWritten = M5.In_I2C.writeRegister8(
      kEs8311Address, kEs8311AdcHpfRegister,
      kEs8311AdcDynamicHpf, kEs8311I2cFrequency);

  uint8_t pgaReadback = 0;
  uint8_t scaleReadback = 0;
  uint8_t volumeReadback = 0;
  uint8_t hpfReadback = 0;
  const bool pgaRead = M5.In_I2C.readRegister(
      kEs8311Address, kEs8311AdcPgaRegister, &pgaReadback,
      sizeof(pgaReadback), kEs8311I2cFrequency);
  const bool scaleRead = M5.In_I2C.readRegister(
      kEs8311Address, kEs8311AdcScaleRegister, &scaleReadback,
      sizeof(scaleReadback), kEs8311I2cFrequency);
  const bool volumeRead = M5.In_I2C.readRegister(
      kEs8311Address, kEs8311AdcVolumeRegister, &volumeReadback,
      sizeof(volumeReadback), kEs8311I2cFrequency);
  const bool hpfRead = M5.In_I2C.readRegister(
      kEs8311Address, kEs8311AdcHpfRegister, &hpfReadback,
      sizeof(hpfReadback), kEs8311I2cFrequency);

  codecBus.restore();
  return pgaWritten && scaleWritten && volumeWritten && hpfWritten && pgaRead &&
         scaleRead && volumeRead && hpfRead &&
         pgaReadback == kEs8311DifferentialInputPga24Db &&
         scaleReadback == kEs8311AdcScale24Db &&
         volumeReadback == kEs8311AdcDigitalPlus6Db &&
         hpfReadback == kEs8311AdcDynamicHpf;
}

bool configureCodecSpeaker() {
  m5gfx::i2c::i2c_temporary_switcher_t codecBus(
      kEs8311I2cPort, kEs8311SdaPin, kEs8311SclPin);

  // This is the pinned M5Unified StopWatch speaker profile. Keep it local to
  // this module because the USB-mic image deliberately starts M5Unified with
  // internal_spk disabled; no USB output terminal is created.
  bool ok = M5.In_I2C.writeRegister8(
      kEs8311Address, kEs8311ResetRegister, 0x80,
      kEs8311I2cFrequency);
  ok = M5.In_I2C.writeRegister8(
           kEs8311Address, kEs8311ClockSourceRegister, 0xB5,
           kEs8311I2cFrequency) &&
       ok;
  ok = M5.In_I2C.writeRegister8(
           kEs8311Address, kEs8311ClockMultiplierRegister, 0x18,
           kEs8311I2cFrequency) &&
       ok;
  ok = M5.In_I2C.writeRegister8(
           kEs8311Address, kEs8311AnalogPowerRegister, 0x01,
           kEs8311I2cFrequency) &&
       ok;
  ok = M5.In_I2C.writeRegister8(
           kEs8311Address, kEs8311DacPowerRegister, 0x00,
           kEs8311I2cFrequency) &&
       ok;
  ok = M5.In_I2C.writeRegister8(
           kEs8311Address, kEs8311OutputEnableRegister, 0x10,
           kEs8311I2cFrequency) &&
       ok;
  ok = M5.In_I2C.writeRegister8(
           kEs8311Address, kEs8311DacVolumeRegister, 0xEF,
           kEs8311I2cFrequency) &&
       ok;
  ok = M5.In_I2C.writeRegister8(
           kEs8311Address, kEs8311DacEqualizerRegister, 0x08,
           kEs8311I2cFrequency) &&
       ok;

  codecBus.restore();
  return ok;
}

void configureLocalSpeaker() {
  auto config = M5.Speaker.config();
  config.pin_mck = kSpeakerMclkPin;
  config.pin_bck = kSpeakerBclkPin;
  config.pin_ws = kSpeakerWordSelectPin;
  config.pin_data_out = kSpeakerDataOutPin;
  config.i2s_port = I2S_NUM_0;
  config.magnification = 4;
  config.sample_rate = 44100;
  config.stereo = true;
  config.buzzer = false;
  config.use_dac = false;
  config.dac_zero_level = 0;
  M5.Speaker.config(config);
  M5.Speaker.setVolume(kChimeVolume);
}

void setAudioPower(bool enabled) {
  auto& ioe1 = M5.getIOExpander(0);
  ioe1.setHighImpedance(kAudioPowerIoePin, false);
  ioe1.setDirection(kAudioPowerIoePin, true);
  ioe1.digitalWrite(kAudioPowerIoePin, enabled);
}

void setSpeakerAmp(bool enabled) {
  auto& ioe1 = M5.getIOExpander(0);
  ioe1.setHighImpedance(kSpeakerAmpIoePin, false);
  ioe1.setDirection(kSpeakerAmpIoePin, true);
  ioe1.digitalWrite(kSpeakerAmpIoePin, enabled);
}

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
  // A warmer A4 -> C#5 major third. Both notes are pure sines with long
  // raised-cosine edges, so the handoff is legato rather than two hard beeps.
  constexpr float kFirstFrequency = 440.00f;
  constexpr float kSecondFrequency = 554.37f;
  constexpr float kFirstDuration = 0.145f;
  constexpr float kSecondStart = 0.105f;
  constexpr float kSecondDuration = 0.165f;
  constexpr float kPeakAmplitude = 0.135f * 32767.0f;

  for (size_t sample = 0; sample < completionChimePcm.size(); ++sample) {
    const float time = static_cast<float>(sample) / kChimeSampleRate;
    const float first =
        sinf(kPi * 2.0f * kFirstFrequency * time) *
        softNoteEnvelope(time, kFirstDuration);
    const float secondTime = time - kSecondStart;
    const float second =
        sinf(kPi * 2.0f * kSecondFrequency * secondTime) *
        softNoteEnvelope(secondTime, kSecondDuration);
    completionChimePcm[sample] =
        static_cast<int16_t>(kPeakAmplitude * (first + second));
  }
}

bool restoreMicrophoneHardware() {
  if (!M5.Mic.begin()) return false;

  // M5.Mic.begin() reapplies its default codec profile. The speech profile is
  // therefore always the final operation, with readback verification.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (configureCodecSpeechGain()) return true;
    vTaskDelay(1);
  }
  M5.Mic.end();
  return false;
}
#endif

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void noteChimeRequest() {
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.chime_requests;
  portEXIT_CRITICAL(&statsMux);
}

void noteChimeBusy() {
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.chime_skipped_busy;
  portEXIT_CRITICAL(&statsMux);
}

void armChimeStatus(bool queued) {
  portENTER_CRITICAL(&statsMux);
  ++chimeSequence;
  latestChimeResult = stopwatch_usb_mic::ChimeResult::Pending;
  if (queued) ++pipelineStats.chime_queued;
  portEXIT_CRITICAL(&statsMux);
}

void finishChime(stopwatch_usb_mic::ChimeResult result) {
  portENTER_CRITICAL(&statsMux);
  latestChimeResult = result;
  switch (result) {
    case stopwatch_usb_mic::ChimeResult::Played:
      ++pipelineStats.chime_played;
      break;
    case stopwatch_usb_mic::ChimeResult::SkippedStreaming:
      ++pipelineStats.chime_skipped_streaming;
      break;
    case stopwatch_usb_mic::ChimeResult::AbortedStreaming:
      ++pipelineStats.chime_aborted_streaming;
      break;
    case stopwatch_usb_mic::ChimeResult::Unavailable:
      ++pipelineStats.chime_unavailable;
      break;
    case stopwatch_usb_mic::ChimeResult::Unsupported:
      ++pipelineStats.chime_unsupported;
      break;
    case stopwatch_usb_mic::ChimeResult::SpeakerStartFailed:
    case stopwatch_usb_mic::ChimeResult::PlaybackFailed:
    case stopwatch_usb_mic::ChimeResult::MicrophoneRestoreFailed:
      ++pipelineStats.chime_failures;
      break;
    case stopwatch_usb_mic::ChimeResult::NeverRequested:
    case stopwatch_usb_mic::ChimeResult::Pending:
      break;
  }
  // Publish idle while the result lock is still held. A new request may then
  // reserve the phase, but cannot publish its sequence/result until this
  // terminal snapshot is complete.
  chimePhase.store(ChimePhase::Idle, std::memory_order_release);
  portEXIT_CRITICAL(&statsMux);
}

bool finishGuardingChime(stopwatch_usb_mic::ChimeResult result) {
  ChimePhase expected = ChimePhase::Guarding;
  if (!chimePhase.compare_exchange_strong(
          expected, ChimePhase::Finishing, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  finishChime(result);
  return true;
}

bool beginChimeHardwareSwitch() {
  ChimePhase expected = ChimePhase::Guarding;
  return chimePhase.compare_exchange_strong(
      expected, ChimePhase::Switching, std::memory_order_acq_rel,
      std::memory_order_acquire);
}

void noteChimeCapturePause() {
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.chime_capture_pauses;
  portEXIT_CRITICAL(&statsMux);
}

stopwatch_usb_mic::Stats readStats() {
  portENTER_CRITICAL(&statsMux);
  stopwatch_usb_mic::Stats result = pipelineStats;
  portEXIT_CRITICAL(&statsMux);
  result.active = usbMicActive.load(std::memory_order_acquire);
  return result;
}

stopwatch_usb_mic::ChimeStatus readChimeStatus() {
  stopwatch_usb_mic::ChimeStatus result = {};
  portENTER_CRITICAL(&statsMux);
  result.sequence = chimeSequence;
  result.result = latestChimeResult;
  const ChimePhase phase = chimePhase.load(std::memory_order_acquire);
  result.pending = phase != ChimePhase::Idle;
  result.playing = phase == ChimePhase::Playing;
  result.stream_requested =
      micInterfaceEnabled.load(std::memory_order_acquire);
  result.microphone_ready =
      captureHardwareReady.load(std::memory_order_acquire) &&
      phase != ChimePhase::Switching && phase != ChimePhase::Playing;
  portEXIT_CRITICAL(&statsMux);
  return result;
}

void noteCaptured(bool inactive, bool stale) {
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.captured_blocks;
  pipelineStats.captured_samples += kSamplesPerBlock;
  if (inactive) ++pipelineStats.inactive_blocks;
  if (stale) ++pipelineStats.stale_blocks;
  portEXIT_CRITICAL(&statsMux);
}

void noteCaptureGap() {
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.capture_gaps;
  ++pipelineStats.discontinuities;
  portEXIT_CRITICAL(&statsMux);
}

void noteQueued() {
  const uint32_t depth = static_cast<uint32_t>(uxQueueMessagesWaiting(audioQueue));
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.queued_blocks;
  if (depth > pipelineStats.queue_high_water) {
    pipelineStats.queue_high_water = depth;
  }
  portEXIT_CRITICAL(&statsMux);
}

void noteQueueDrop() {
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.queue_drops;
  ++pipelineStats.discontinuities;
  portEXIT_CRITICAL(&statsMux);
}

void noteWrite(uint16_t requested, uint16_t written) {
  portENTER_CRITICAL(&statsMux);
  if (written == 0) {
    ++pipelineStats.write_zero;
  } else {
    pipelineStats.written_bytes += written;
    if (written < requested) ++pipelineStats.write_short;
  }
  portEXIT_CRITICAL(&statsMux);
}

void noteStartupTopUp(const SilenceFillResult& result) {
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.startup_topups;
  pipelineStats.startup_topup_calls += result.calls;
  pipelineStats.startup_topup_bytes += result.bytes;
  portEXIT_CRITICAL(&statsMux);
}

void noteQueueUnderrun(const SilenceFillResult& result) {
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.queue_underruns;
  pipelineStats.underrun_silence_calls += result.calls;
  pipelineStats.underrun_silence_bytes += result.bytes;
  ++pipelineStats.discontinuities;
  portEXIT_CRITICAL(&statsMux);
}

uint16_t flowControlTarget(tu_fifo_t* fifo) {
  if (fifo == nullptr) return 0;
  const uint16_t halfDepth = tu_fifo_depth(fifo) / 2U;
  uint16_t target = tud_audio_get_ep_in_fifo_threshold();
  if (target == 0 || target > halfDepth) target = halfDepth;
  return target;
}

bool blockBelongsToActiveStream(uint32_t generation);

void prefillStartupSilence() {
  tu_fifo_t* fifo = tud_audio_get_ep_in_ff();
  uint16_t target = 0;
  uint32_t calls = 0;
  uint64_t acceptedBytes = 0;

  if (fifo != nullptr) {
    target = flowControlTarget(fifo);

    while (tu_fifo_count(fifo) < target &&
           calls < kStartupPrefillWriteLimit) {
      const uint16_t remaining = target - tu_fifo_count(fifo);
      const uint16_t request = static_cast<uint16_t>(
          min<size_t>(remaining, sizeof(silenceBlock)));
      const uint16_t accepted = audioCard.write(silenceBlock, request);
      const uint16_t consumed = min<uint16_t>(accepted, request);
      ++calls;
      noteWrite(request, consumed);
      acceptedBytes += consumed;
      if (consumed == 0) break;
    }
  }

  const uint16_t finalCount = fifo == nullptr ? 0 : tu_fifo_count(fifo);
  const uint16_t shortfall = finalCount < target ? target - finalCount : 0;
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.startup_prefills;
  pipelineStats.startup_prefill_calls += calls;
  pipelineStats.startup_prefill_bytes += acceptedBytes;
  pipelineStats.startup_prefill_shortfall_bytes += shortfall;
  if (shortfall != 0 && calls == kStartupPrefillWriteLimit) {
    ++pipelineStats.startup_prefill_limit_hits;
  }
  portEXIT_CRITICAL(&statsMux);
}

SilenceFillResult topUpSilence(uint32_t generation,
                               bool requireStartupPending) {
  SilenceFillResult result = {};
  if (!blockBelongsToActiveStream(generation)) return result;

  xSemaphoreTake(usbFifoMutex, portMAX_DELAY);
  if (!blockBelongsToActiveStream(generation) ||
      (requireStartupPending &&
       startupAwaitingGeneration.load(std::memory_order_acquire) !=
           generation)) {
    xSemaphoreGive(usbFifoMutex);
    return result;
  }

  tu_fifo_t* fifo = tud_audio_get_ep_in_ff();
  const uint16_t target = flowControlTarget(fifo);
  while (fifo != nullptr && tu_fifo_count(fifo) < target &&
         result.calls < kStartupPrefillWriteLimit) {
    const uint16_t remaining = target - tu_fifo_count(fifo);
    const uint16_t request = static_cast<uint16_t>(
        min<size_t>(remaining, sizeof(silenceBlock)));
    const uint16_t accepted = audioCard.write(silenceBlock, request);
    const uint16_t consumed = min<uint16_t>(accepted, request);
    ++result.calls;
    result.bytes += consumed;
    noteWrite(request, consumed);
    if (consumed == 0) break;
  }
  xSemaphoreGive(usbFifoMutex);
  return result;
}

void noteAbandoned(size_t bytes) {
  if (bytes == 0) return;
  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.abandoned_blocks;
  pipelineStats.abandoned_bytes += bytes;
  ++pipelineStats.discontinuities;
  portEXIT_CRITICAL(&statsMux);
}

size_t clearQueuedAudio() {
  if (audioQueue == nullptr) return 0;
  size_t cleared = 0;
  while (xQueueReceive(audioQueue, &transitionDropBlock, 0) == pdTRUE) {
    ++cleared;
  }
  return cleared;
}

void transitionStreaming(bool active, bool interfaceTransition) {
  const bool previous = usbMicActive.load(std::memory_order_acquire);
  if (previous == active && !interfaceTransition) return;

  // Publish inactive first, then invalidate all blocks before touching either
  // FIFO. The feeder also checks the generation, so it cannot resume a partial
  // block if a fast disable/enable pair occurs between its checks.
  usbMicActive.store(false, std::memory_order_release);
  const uint32_t generation =
      streamGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
  const size_t cleared = clearQueuedAudio();
  // Serialize the clear with write(). Without this lock, a write that began
  // just before the transition could refill the FIFO after it was cleared.
  if (usbFifoMutex != nullptr) {
    xSemaphoreTake(usbFifoMutex, portMAX_DELAY);
    tud_audio_clear_ep_in_ff();
    if (active && static_cast<bool>(USB)) prefillStartupSilence();
    xSemaphoreGive(usbFifoMutex);
  }

  portENTER_CRITICAL(&statsMux);
  ++pipelineStats.stream_transitions;
  if (interfaceTransition) ++pipelineStats.interface_transitions;
  pipelineStats.transition_cleared_blocks += cleared;
  if (cleared != 0) ++pipelineStats.discontinuities;
  portEXIT_CRITICAL(&statsMux);

  startupAwaitingGeneration.store(active ? generation : 0,
                                  std::memory_order_release);
  usbMicActive.store(active, std::memory_order_release);
}

void recomputeStreaming(bool interfaceTransition) {
  const bool active =
      micInterfaceEnabled.load(std::memory_order_acquire) &&
      usbLinkUsable.load(std::memory_order_acquire);
  transitionStreaming(active, interfaceTransition);
}

void audioEventCallback(void*, esp_event_base_t eventBase, int32_t eventId,
                        void* eventData) {
  if (eventBase != ARDUINO_USB_AUDIO_CARD_EVENTS ||
      eventId != ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT ||
      eventData == nullptr) {
    return;
  }
  const auto* data =
      static_cast<const arduino_usb_audio_card_event_data_t*>(eventData);
  if (data->interface_enable.interface != UAC_INTERFACE_MIC) return;

  const bool enabled = data->interface_enable.enable;
  const bool previous =
      micInterfaceEnabled.exchange(enabled, std::memory_order_acq_rel);
  if (enabled) {
    // Publish the level before the generation. A requester that sees the old
    // generation must then see either the asserted level or the later
    // generation increment; a fast alt1 -> alt0 pulse remains preserved.
    micInterfaceEnableGeneration.fetch_add(1, std::memory_order_acq_rel);
  }
  if (previous != enabled) recomputeStreaming(true);
}

void usbEventCallback(void*, esp_event_base_t eventBase, int32_t eventId,
                      void*) {
  if (eventBase != ARDUINO_USB_EVENTS) return;

  bool usable;
  switch (eventId) {
    case ARDUINO_USB_STARTED_EVENT:
    case ARDUINO_USB_RESUME_EVENT:
      usable = true;
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      // A new enumeration starts with alternate setting zero; do not carry the
      // previous connection's microphone setting into ARDUINO_USB_STARTED.
      micInterfaceEnabled.store(false, std::memory_order_release);
      usable = false;
      break;
    case ARDUINO_USB_SUSPEND_EVENT:
      usable = false;
      break;
    default:
      return;
  }
  const bool previous = usbLinkUsable.exchange(usable, std::memory_order_acq_rel);
  if (previous != usable) recomputeStreaming(false);
}

bool blockBelongsToActiveStream(uint32_t generation) {
  return usbMicActive.load(std::memory_order_acquire) &&
         streamGeneration.load(std::memory_order_acquire) == generation;
}

bool snapshotActiveGeneration(uint32_t* generation) {
  const uint32_t before =
      streamGeneration.load(std::memory_order_acquire);
  const bool active = usbMicActive.load(std::memory_order_acquire);
  const uint32_t after =
      streamGeneration.load(std::memory_order_acquire);
  *generation = after;
  return active && before == after;
}

bool enqueueLatest(const int16_t* samples, uint32_t generation) {
  if (!blockBelongsToActiveStream(generation)) return false;

  enqueueBlock.generation = generation;
  memcpy(enqueueBlock.samples, samples, kBlockBytes);

  while (xQueueSend(audioQueue, &enqueueBlock, 0) != pdTRUE) {
    if (!blockBelongsToActiveStream(generation)) return false;
    // A full queue is always resolved by evicting its oldest complete block.
    // There is one producer, so the following send must eventually succeed;
    // a concurrent consumer or transition can only create more free space.
    if (xQueueReceive(audioQueue, &producerDropBlock, 0) == pdTRUE) {
      noteQueueDrop();
    } else {
      taskYIELD();
    }
  }
  noteQueued();
  return true;
}

bool writeBlock(const AudioBlock& block) {
  const uint8_t* cursor =
      reinterpret_cast<const uint8_t*>(block.samples);
  size_t remaining = kBlockBytes;

  while (remaining != 0) {
    if (!blockBelongsToActiveStream(block.generation)) {
      noteAbandoned(remaining);
      return false;
    }

    const uint16_t request = static_cast<uint16_t>(remaining);
    xSemaphoreTake(usbFifoMutex, portMAX_DELAY);
    if (!blockBelongsToActiveStream(block.generation)) {
      xSemaphoreGive(usbFifoMutex);
      noteAbandoned(remaining);
      return false;
    }
    const uint16_t accepted = audioCard.write(cursor, request);
    xSemaphoreGive(usbFifoMutex);

    const uint16_t consumed = min<uint16_t>(accepted, request);
    noteWrite(request, consumed);
    if (consumed == 0) {
      vTaskDelay(1);
      continue;
    }

    cursor += consumed;
    remaining -= consumed;
  }
  return true;
}

void feedUsb(void*) {
  for (;;) {
    uint32_t generation;
    if (!snapshotActiveGeneration(&generation)) {
      vTaskDelay(pdMS_TO_TICKS(kBlockDurationMs));
      continue;
    }

    const bool awaitingFresh =
        startupAwaitingGeneration.load(std::memory_order_acquire) == generation;
    const TickType_t waitTicks = pdMS_TO_TICKS(
        awaitingFresh ? kStartupTopUpPollMs : kBlockDurationMs * 2);
    if (xQueueReceive(audioQueue, &usbTransferBlock,
                      waitTicks) != pdTRUE) {
      if (!blockBelongsToActiveStream(generation)) continue;
      const SilenceFillResult fill = topUpSilence(generation, awaitingFresh);
      if (awaitingFresh) {
        noteStartupTopUp(fill);
      } else {
        // A source block has not arrived for two complete block periods. Keep
        // USB packet cadence continuous with bounded silence and surface the
        // source starvation independently of USB short-write counters.
        noteQueueUnderrun(fill);
      }
      continue;
    }
    if (!blockBelongsToActiveStream(usbTransferBlock.generation)) {
      portENTER_CRITICAL(&statsMux);
      ++pipelineStats.stale_blocks;
      portEXIT_CRITICAL(&statsMux);
      continue;
    }
    uint32_t expectedGeneration = usbTransferBlock.generation;
    startupAwaitingGeneration.compare_exchange_strong(
        expectedGeneration, 0, std::memory_order_acq_rel,
        std::memory_order_acquire);
    writeBlock(usbTransferBlock);
  }
}

#if CODEX_STOPWATCH_USB_MIC_SYNTHETIC

constexpr int16_t kSynthetic1kHz[48] = {
    0,     1566,  3106,  4592,  6000,  7305,  8485,  9518,
    10392, 11087, 11591, 11897, 12000, 11897, 11591, 11087,
    10392, 9518,  8485,  7305,  6000,  4592,  3106,  1566,
    0,     -1566, -3106, -4592, -6000, -7305, -8485, -9518,
    -10392, -11087, -11591, -11897, -12000, -11897, -11591,
    -11087, -10392, -9518, -8485, -7305, -6000, -4592, -3106,
    -1566,
};

void captureAudio(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    uint32_t generation;
    const bool startedActive = snapshotActiveGeneration(&generation);
    for (size_t i = 0; i < kSamplesPerBlock; ++i) {
      captureBuffers[0][i] = kSynthetic1kHz[i % 48];
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(kBlockDurationMs));

    const bool active = usbMicActive.load(std::memory_order_acquire);
    const bool stale =
        !startedActive ||
        streamGeneration.load(std::memory_order_acquire) != generation;
    noteCaptured(!startedActive || !active, stale);
    if (startedActive && active && !stale) {
      enqueueLatest(captureBuffers[0], generation);
    }
  }
}

#else

bool streamRequestedDuringChime() {
  return micInterfaceEnabled.load(std::memory_order_acquire) ||
         micInterfaceEnableGeneration.load(std::memory_order_acquire) !=
             chimeAlt1Baseline.load(std::memory_order_acquire);
}

void markCaptureHardwareFailed() {
  captureHardwareReady.store(false, std::memory_order_release);

  // If failure races request arming, wait for the requester to publish either
  // Guarding or a terminal Idle state. This prevents an Arming observation
  // from being the last word before the capture task exits.
  ChimePhase phase = chimePhase.load(std::memory_order_acquire);
  while (phase == ChimePhase::Arming) {
    vTaskDelay(1);
    phase = chimePhase.load(std::memory_order_acquire);
  }
  if (phase == ChimePhase::Guarding) {
    finishGuardingChime(stopwatch_usb_mic::ChimeResult::Unavailable);
  }
}

bool queueInitialCaptureBlocks(size_t* nextBuffer) {
  *nextBuffer = 0;
  while (M5.Mic.isRecording() < 2) {
    captureStartedActive[*nextBuffer] =
        snapshotActiveGeneration(&captureGenerations[*nextBuffer]);
    if (!M5.Mic.record(captureBuffers[*nextBuffer], kSamplesPerBlock,
                       kSampleRate)) {
      return false;
    }
    *nextBuffer ^= 1;
  }
  return true;
}

void stopLocalSpeaker() {
  M5.Speaker.stop();
  setSpeakerAmp(false);
  M5.Speaker.end();
  setAudioPower(false);
}

bool restartCaptureAfterChime(size_t* nextBuffer) {
  if (!restoreMicrophoneHardware()) return false;
  if (!queueInitialCaptureBlocks(nextBuffer)) {
    M5.Mic.end();
    return false;
  }
  return true;
}

bool performCompletionChime(size_t* nextBuffer) {
  noteChimeCapturePause();

  // Stop replenishing M5Unified's two capture slots, then let both finish.
  // This avoids deleting its I2S task while a DMA destination is still live.
  while (M5.Mic.isRecording() != 0) vTaskDelay(1);

  if (streamRequestedDuringChime()) {
    if (!queueInitialCaptureBlocks(nextBuffer)) {
      captureHardwareReady.store(false, std::memory_order_release);
      finishChime(
          stopwatch_usb_mic::ChimeResult::MicrophoneRestoreFailed);
      return false;
    }
    finishChime(stopwatch_usb_mic::ChimeResult::SkippedStreaming);
    return true;
  }

  M5.Mic.end();

  // An alt1 request may race the last capture block. Do not even enable the
  // amplifier in that case; put the microphone back first.
  if (streamRequestedDuringChime()) {
    const bool restored = restartCaptureAfterChime(nextBuffer);
    if (!restored) {
      captureHardwareReady.store(false, std::memory_order_release);
      finishChime(
          stopwatch_usb_mic::ChimeResult::MicrophoneRestoreFailed);
      return false;
    }
    finishChime(stopwatch_usb_mic::ChimeResult::SkippedStreaming);
    return true;
  }

  setAudioPower(true);
  vTaskDelay(pdMS_TO_TICKS(10));
  const bool codecReady = configureCodecSpeaker();
  bool speakerReady = false;
  if (codecReady && !streamRequestedDuringChime()) {
    setSpeakerAmp(true);
    // Avoid starting the I2S speaker task if alt1 arrived while the amplifier
    // enable write was in flight. A later arrival is caught before any PCM is
    // queued and again on every playback poll.
    if (!streamRequestedDuringChime()) {
      speakerReady = M5.Speaker.begin() && M5.Speaker.isRunning();
    }
  }

  stopwatch_usb_mic::ChimeResult result =
      stopwatch_usb_mic::ChimeResult::SpeakerStartFailed;
  if (speakerReady && !streamRequestedDuringChime()) {
    M5.Speaker.setVolume(kChimeVolume);
    const bool queued = M5.Speaker.playRaw(
        completionChimePcm.data(), completionChimePcm.size(),
        kChimeSampleRate, false, 1, -1, true);
    if (queued && M5.Speaker.isPlaying()) {
      chimePhase.store(ChimePhase::Playing, std::memory_order_release);
      const uint32_t startedMs = millis();
      while (M5.Speaker.isPlaying() &&
             millis() - startedMs < kChimePlaybackTimeoutMs) {
        if (streamRequestedDuringChime()) break;
        vTaskDelay(1);
      }
      if (streamRequestedDuringChime()) {
        result = stopwatch_usb_mic::ChimeResult::AbortedStreaming;
      } else if (M5.Speaker.isPlaying()) {
        result = stopwatch_usb_mic::ChimeResult::PlaybackFailed;
      } else {
        result = stopwatch_usb_mic::ChimeResult::Played;
        const uint32_t drainStartedMs = millis();
        while (millis() - drainStartedMs < kChimeDmaDrainMs) {
          if (streamRequestedDuringChime()) {
            result = stopwatch_usb_mic::ChimeResult::AbortedStreaming;
            break;
          }
          vTaskDelay(1);
        }
      }
    } else {
      result = stopwatch_usb_mic::ChimeResult::PlaybackFailed;
    }
  } else if (streamRequestedDuringChime()) {
    result = stopwatch_usb_mic::ChimeResult::SkippedStreaming;
  }

  stopLocalSpeaker();
  const bool restored = restartCaptureAfterChime(nextBuffer);
  if (!restored) {
    captureHardwareReady.store(false, std::memory_order_release);
    finishChime(stopwatch_usb_mic::ChimeResult::MicrophoneRestoreFailed);
    return false;
  }

  // A request arriving while the codec was changing modes still counts as an
  // abort even if the PCM task had just reached its natural end.
  if (streamRequestedDuringChime() &&
      result == stopwatch_usb_mic::ChimeResult::Played) {
    result = stopwatch_usb_mic::ChimeResult::AbortedStreaming;
  }
  finishChime(result);
  return true;
}

void captureAudio(void*) {
  // M5Unified exposes two application capture slots. Their generation records
  // when capture started, preventing a block that straddles enable/resume from
  // becoming the first block of a new stream.
  size_t nextBuffer = 0;
  if (!queueInitialCaptureBlocks(&nextBuffer)) {
    Serial.println("USB mic capture queue failed");
    markCaptureHardwareFailed();
    captureTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    const ChimePhase phase = chimePhase.load(std::memory_order_acquire);
    if (phase == ChimePhase::Guarding) {
      if (streamRequestedDuringChime()) {
        finishGuardingChime(
            stopwatch_usb_mic::ChimeResult::SkippedStreaming);
      } else if (deadlineReached(
                     millis(),
                     chimeGuardUntilMs.load(std::memory_order_acquire))) {
        if (beginChimeHardwareSwitch() &&
            !performCompletionChime(&nextBuffer)) {
          Serial.println("USB mic restore after completion chime failed");
          captureTask = nullptr;
          vTaskDelete(nullptr);
          return;
        }
        continue;
      }
    }

    size_t recordingCount;
    while ((recordingCount = M5.Mic.isRecording()) >= 2) {
      const ChimePhase waitingPhase =
          chimePhase.load(std::memory_order_acquire);
      if (waitingPhase == ChimePhase::Guarding &&
          (streamRequestedDuringChime() ||
           deadlineReached(
               millis(),
               chimeGuardUntilMs.load(std::memory_order_acquire)))) {
        break;
      }
      vTaskDelay(1);
    }

    if (chimePhase.load(std::memory_order_acquire) == ChimePhase::Guarding &&
        (streamRequestedDuringChime() ||
         deadlineReached(
             millis(),
             chimeGuardUntilMs.load(std::memory_order_acquire)))) {
      continue;
    }
    if (recordingCount >= 2) continue;
    if (recordingCount == 0) noteCaptureGap();

    const uint32_t completedGeneration = captureGenerations[nextBuffer];
    const bool startedActive = captureStartedActive[nextBuffer];
    const bool active = usbMicActive.load(std::memory_order_acquire);
    const bool stale =
        !startedActive ||
        streamGeneration.load(std::memory_order_acquire) !=
            completedGeneration;
    noteCaptured(!startedActive || !active, stale);
    if (startedActive && active && !stale) {
      enqueueLatest(captureBuffers[nextBuffer], completedGeneration);
    }

    captureStartedActive[nextBuffer] =
        snapshotActiveGeneration(&captureGenerations[nextBuffer]);
    if (!M5.Mic.record(captureBuffers[nextBuffer], kSamplesPerBlock,
                       kSampleRate)) {
      Serial.println("USB mic capture requeue failed");
      markCaptureHardwareFailed();
      captureTask = nullptr;
      vTaskDelete(nullptr);
      return;
    }
    nextBuffer ^= 1;
  }
}

#endif

void stopPipeline() {
  usbMicActive.store(false, std::memory_order_release);
  captureHardwareReady.store(false, std::memory_order_release);
  startupAwaitingGeneration.store(0, std::memory_order_release);
  if (captureTask != nullptr) {
    vTaskDelete(captureTask);
    captureTask = nullptr;
  }
  if (usbTask != nullptr) {
    vTaskDelete(usbTask);
    usbTask = nullptr;
  }
  if (audioQueue != nullptr) {
    vQueueDelete(audioQueue);
    audioQueue = nullptr;
  }
  if (usbFifoMutex != nullptr) {
    vSemaphoreDelete(usbFifoMutex);
    usbFifoMutex = nullptr;
  }
#if !CODEX_STOPWATCH_USB_MIC_SYNTHETIC
  M5.Mic.end();
  stopLocalSpeaker();
#endif
  audioCard.end();
  pipelineStarted.store(false, std::memory_order_release);
  chimePhase.store(ChimePhase::Idle, std::memory_order_release);
}

}  // namespace

namespace stopwatch_usb_mic {

bool begin() {
  if (pipelineStarted.exchange(true, std::memory_order_acq_rel)) return true;

  M5.Speaker.end();

#if !CODEX_STOPWATCH_USB_MIC_SYNTHETIC
  configureLocalSpeaker();
  buildCompletionChime();

  auto micConfig = M5.Mic.config();
  micConfig.sample_rate = kSampleRate;
  micConfig.input_channel = m5::input_only_right;
  micConfig.dma_buf_len = kDmaFrames;
  micConfig.dma_buf_count = 4;
  micConfig.over_sampling = 1;
  // M5Unified divides magnification by (over_sampling * 2), so 2 is exactly
  // 1x software gain here. Codec gain is configured explicitly below.
  micConfig.magnification = 2;
  micConfig.task_priority = kCaptureTaskPriority;
  M5.Mic.config(micConfig);
  if (!M5.Mic.begin()) {
    Serial.println("USB mic codec capture failed");
    pipelineStarted.store(false, std::memory_order_release);
    return false;
  }
  // M5Unified records its effective sample rate only when record() observes a
  // running task with a changed rate. Prime one discarded block so that
  // one-time internal restart happens now, then restart cleanly before applying
  // the ES8311 speech gain. Without this step, the first real record() would
  // overwrite our codec registers with M5Unified's default gain profile.
  if (!M5.Mic.record(captureBuffers[0], kSamplesPerBlock, kSampleRate)) {
    Serial.println("USB mic codec capture prime failed");
    M5.Mic.end();
    pipelineStarted.store(false, std::memory_order_release);
    return false;
  }
  const uint32_t primeStartedMs = millis();
  while (M5.Mic.isRecording() != 0 && millis() - primeStartedMs < 100) {
    vTaskDelay(1);
  }
  if (M5.Mic.isRecording() != 0) {
    Serial.println("USB mic codec capture prime timed out");
    M5.Mic.end();
    pipelineStarted.store(false, std::memory_order_release);
    return false;
  }
  M5.Mic.end();
  if (!M5.Mic.begin()) {
    Serial.println("USB mic codec capture restart failed");
    pipelineStarted.store(false, std::memory_order_release);
    return false;
  }
  if (!configureCodecSpeechGain()) {
    Serial.println("USB mic codec gain configuration failed");
    M5.Mic.end();
    pipelineStarted.store(false, std::memory_order_release);
    return false;
  }
  captureHardwareReady.store(true, std::memory_order_release);
#endif

  audioQueue = xQueueCreateStatic(kQueuedBlocks, sizeof(AudioBlock),
                                  audioQueueStorage, &audioQueueControl);
  if (audioQueue == nullptr) {
    Serial.println("USB mic audio queue allocation failed");
#if !CODEX_STOPWATCH_USB_MIC_SYNTHETIC
    M5.Mic.end();
#endif
    pipelineStarted.store(false, std::memory_order_release);
    return false;
  }
  usbFifoMutex = xSemaphoreCreateMutexStatic(&usbFifoMutexControl);
  if (usbFifoMutex == nullptr) {
    Serial.println("USB mic FIFO mutex allocation failed");
    stopPipeline();
    return false;
  }

  audioCard.onEvent(ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT,
                    audioEventCallback);
  USB.onEvent(usbEventCallback);
  if (!audioCard.begin()) {
    Serial.println("USB mic audio class start failed");
    stopPipeline();
    return false;
  }

  if (xTaskCreate(feedUsb, "usb_mic_tx", kTaskStackBytes, nullptr,
                  kUsbTaskPriority, &usbTask) != pdPASS) {
    Serial.println("USB mic feeder task failed");
    stopPipeline();
    return false;
  }
  if (xTaskCreate(captureAudio, "usb_mic_capture", kTaskStackBytes, nullptr,
                  kCaptureTaskPriority, &captureTask) != pdPASS) {
    Serial.println("USB mic capture task failed");
    stopPipeline();
    return false;
  }

  USB.VID(0x303A);
  USB.PID(0x0002);
  USB.productName("Codex StopWatch Mic");
  USB.manufacturerName("Codex Micro StopWatch");
  USB.serialNumber("stopwatch-mic");
  USB.firmwareVersion(0x0100);
  USB.usbVersion(0x0200);
  USB.usbClass(0);
  USB.usbSubClass(0);
  USB.usbProtocol(0);
  USB.usbPower(500);
  USB.webUSB(false);
  if (!USB.begin()) {
    Serial.println("USB mic TinyUSB start failed");
    stopPipeline();
    return false;
  }

  // Do not override TinyUSB's half-FIFO flow-control target. Each active
  // transition pre-fills silence only to that target (never three quarters),
  // then the first fresh 10 ms block continues the stream.
  Serial.printf(
      "USB_MIC_READY rate=48000 bits=16 channels=1 speaker=0 block_ms=10 "
      "queue_blocks=2 prefill_write_limit=%lu pga_db=24 adc_scale_db=24 "
      "adc_volume_db=6 software_gain=1 codec_hpf=dynamic "
      "m5_dc_servo=disabled local_chime=%u chime_guard_ms=%lu "
      "synthetic=%u\n",
      static_cast<unsigned long>(kStartupPrefillWriteLimit),
      CODEX_STOPWATCH_USB_MIC_SYNTHETIC ? 0U : 1U,
      static_cast<unsigned long>(kChimeIdleGuardMs),
      CODEX_STOPWATCH_USB_MIC_SYNTHETIC ? 1U : 0U);
  return true;
}

ChimeRequestResult requestCompletionChime() {
  const uint32_t alt1Baseline =
      micInterfaceEnableGeneration.load(std::memory_order_acquire);
  noteChimeRequest();

  ChimePhase expected = ChimePhase::Idle;
  if (!chimePhase.compare_exchange_strong(
          expected, ChimePhase::Arming, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    noteChimeBusy();
    return ChimeRequestResult::Busy;
  }

  chimeAlt1Baseline.store(alt1Baseline, std::memory_order_release);

#if CODEX_STOPWATCH_USB_MIC_SYNTHETIC
  armChimeStatus(false);
  finishChime(ChimeResult::Unsupported);
  return ChimeRequestResult::Unsupported;
#else
  if (!pipelineStarted.load(std::memory_order_acquire) ||
      !captureHardwareReady.load(std::memory_order_acquire)) {
    armChimeStatus(false);
    finishChime(ChimeResult::Unavailable);
    return ChimeRequestResult::Unavailable;
  }

  if (streamRequestedDuringChime()) {
    armChimeStatus(false);
    finishChime(ChimeResult::SkippedStreaming);
    return ChimeRequestResult::SkippedStreaming;
  }

  chimeGuardUntilMs.store(millis() + kChimeIdleGuardMs,
                          std::memory_order_release);
  armChimeStatus(true);
  // An alt1 transition between the baseline snapshot and this publish remains
  // visible through the monotonic interface-enable generation.
  chimePhase.store(ChimePhase::Guarding, std::memory_order_release);

  // Close the capture-task failure race around Arming -> Guarding. Both this
  // path and the capture task must claim Guarding -> Finishing, so only one can
  // publish the terminal result.
  if (!captureHardwareReady.load(std::memory_order_acquire)) {
    finishGuardingChime(ChimeResult::Unavailable);
    return ChimeRequestResult::Unavailable;
  }
  if (streamRequestedDuringChime()) {
    finishGuardingChime(ChimeResult::SkippedStreaming);
    return ChimeRequestResult::SkippedStreaming;
  }
  return ChimeRequestResult::Queued;
#endif
}

ChimeStatus snapshotChimeStatus() { return readChimeStatus(); }

Stats snapshotStats() { return readStats(); }

}  // namespace stopwatch_usb_mic

#endif  // CODEX_STOPWATCH_USB_MIC
