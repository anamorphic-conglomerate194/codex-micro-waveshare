// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo
// StopWatch port changes copyright (c) 2026 Codex Micro for StopWatch contributors

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLECharacteristic.h>
#include <BLEHIDDevice.h>
#include <BLEServer.h>
#include <freertos/queue.h>

#include <array>
#include <atomic>

#include "ConnectionHealth.h"

struct ThreadLight {
  uint32_t color = 0;
  float brightness = 0.0f;
  String effect = "off";
  float speed = 0.0f;
};

struct QuotaState {
  float remainingPercent = 0.0f;
  uint32_t resetInSeconds = 0;
  uint32_t receivedAtMs = 0;
  bool available = false;
};

struct CodexMicroState {
  std::array<ThreadLight, 6> threads;
  QuotaState quota;
  bool connected = false;
  bool hostRpcObserved = false;
  uint32_t lastHostRpcAtMs = 0;
  uint32_t connectionEpoch = 0;
  bool dirty = true;
};

class CodexMicroBle {
 public:
  static constexpr uint16_t kVendorId = 0x303A;
  static constexpr uint16_t kProductId = 0x8360;
  static constexpr uint8_t kReportId = 6;

  void begin();
  void poll();
  void setBattery(uint8_t percentage, bool charging);
  void sendKey(const char* key, uint8_t action, int8_t agent = -1);
  void sendJoystick(float angle, float distance);
  bool connected();
  CodexMicroState snapshot();

 private:
  class ServerCallbacks;
  class OutputCallbacks;
  class QuotaCallbacks;

  using PeerAddress = std::array<uint8_t, 6>;

  struct PendingOutputReport {
    uint8_t length = 0;
    uint16_t connectionId = 0;
    PeerAddress peerAddress = {};
    std::array<uint8_t, 64> data = {};
  };

  struct PendingConnectionEvent {
    uint16_t id = 0;
    bool connected = false;
  };

  struct PendingQuotaWrite {
    uint16_t length = 0;
    bool responseExpected = false;
    PeerAddress peerAddress = {};
    std::array<uint8_t, 512> data = {};
  };

  void onConnectionEvent(bool connected, uint16_t id);
  void processConnectionEvents();
  void applyConnectionEvent(const PendingConnectionEvent& event);
  void reconcileConnectionSet();
  void clearHostRpcIdentity();
  void noteHostRpcActivity(uint16_t connectionId,
                           const PeerAddress& peerAddress);
  void onOutput(const uint8_t* data, size_t length, uint16_t connectionId,
                const uint8_t* peerAddress);
  void processOutput(const uint8_t* data, size_t length,
                     uint16_t connectionId, const PeerAddress& peerAddress);
  void onQuotaWrite(const uint8_t* data, size_t length,
                    bool responseExpected, const uint8_t* peerAddress);
  void processQuotaWrites();
  void processQuotaWrite(const uint8_t* data, size_t length,
                         bool responseExpected,
                         const PeerAddress& peerAddress);
#if defined(CODEX_STOPWATCH_USB_MIC)
  bool isBootloaderRequest(const uint8_t* data, size_t length) const;
  bool isBootloaderPeerAuthorized(const PeerAddress& peerAddress) const;
  bool usbPowerPresent() const;
  void processPendingBootloaderRestart();
#endif
  bool handleRpc(const JsonDocument& request);
  void sendResult(JsonVariantConst id, JsonVariantConst result);
  void sendSuccess(JsonVariantConst id);
  void sendJson(const String& json);
  void updateThreadLighting(JsonArrayConst values);

  BLEHIDDevice* hid_ = nullptr;
  BLEServer* server_ = nullptr;
  BLEService* hidService_ = nullptr;
  BLECharacteristic* input_ = nullptr;
  BLECharacteristic* battery_ = nullptr;
  SemaphoreHandle_t stateMutex_ = nullptr;
  QueueHandle_t outputQueue_ = nullptr;
  QueueHandle_t connectionEventQueue_ = nullptr;
  QueueHandle_t quotaWriteQueue_ = nullptr;
  CodexMicroState state_;
  String rpcBuffer_;
  uint16_t rpcBufferConnectionId_ = 0;
  bool rpcBufferConnectionValid_ = false;
  uint16_t hostRpcConnectionId_ = 0;
  bool hostRpcConnectionValid_ = false;
  uint8_t batteryPercentage_ = 100;
  bool charging_ = false;
  connection_health::ConnectionSet connections_;
  std::atomic<bool> connectionEventLost_{false};
#if defined(CODEX_STOPWATCH_USB_MIC)
  PeerAddress hostRpcPeerAddress_ = {};
  uint32_t hostRpcPeerEpoch_ = 0;
  bool hostRpcPeerValid_ = false;
  bool bootloaderRestartPending_ = false;
  uint32_t bootloaderRestartAtMs_ = 0;
#endif
};
