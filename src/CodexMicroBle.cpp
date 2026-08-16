// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo
// StopWatch port changes copyright (c) 2026 Codex Micro for StopWatch contributors

#include "CodexMicroBle.h"

#include "HostRpcRequest.h"
#include "QuotaPayload.h"

#include <BLEDevice.h>
#include <BLE2902.h>
#include <BLE2904.h>
#include <BLEDescriptor.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#if defined(CODEX_STOPWATCH_USB_MIC)
#include <M5Unified.h>
#include <esp32-hal-tinyusb.h>
#endif
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <esp_gap_ble_api.h>
#endif
#if defined(CODEX_STOPWATCH_USB_MIC) && defined(CONFIG_BLUEDROID_ENABLED)
#include <Preferences.h>
#endif

namespace {

constexpr char kDeviceName[] = "Codex Micro";
constexpr char kManufacturer[] = "Work Louder";
constexpr char kFirmwareVersion[] = "0.1.0-stopwatch-port";
constexpr char kQuotaServiceUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01";
constexpr char kQuotaWriteUuid[] = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02";
// CODEX_STOPWATCH_THIRD_PARTY: the vendor button/joystick JSON rides the real
// HID over GATT service 0x1812 (report ID 6) so Windows can pair the watch as
// an HID device and the ChatGPT app's node-hid integration consumes the
// reports natively. This UUID is retained for the host bridge's fallback
// subscription, but the firmware no longer exposes it as a service.
constexpr char kControlServiceUuid[] = "5f9d4e66-2ac2-4a71-bfbe-4ef61a0e5c11";
constexpr size_t kPayloadSize = 61;
constexpr size_t kReportBodySize = 63;
#if defined(CODEX_STOPWATCH_USB_MIC)
constexpr int16_t kBootloaderMinimumVbusMv = 4000;
constexpr uint32_t kBootloaderRestartDelayMs = 400;
constexpr char kBootloaderRequest[] =
    "{\"op\":\"enter_bootloader\",\"version\":1,\"confirm\":true}";
#endif

constexpr uint16_t swapBytes(uint16_t value) {
  return static_cast<uint16_t>((value << 8) | (value >> 8));
}

// One vendor-defined input/output report. HIDAPI adds/removes Report ID 6,
// while the BLE characteristics carry the remaining 63-byte report body.
const uint8_t kReportMap[] = {
    0x06, 0x00, 0xFF,        // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,              // Usage (1)
    0xA1, 0x01,              // Collection (Application)
    0x85, 0x06,              // Report ID (6)
    0x15, 0x00,              // Logical Minimum (0)
    0x26, 0xFF, 0x00,        // Logical Maximum (255)
    0x75, 0x08,              // Report Size (8)
    0x95, 0x3F,              // Report Count (63)
    0x09, 0x01,              // Usage (1)
    0x81, 0x02,              // Input (Data, Variable, Absolute)
    0x95, 0x3F,              // Report Count (63)
    0x09, 0x02,              // Usage (2)
    0x91, 0x02,              // Output (Data, Variable, Absolute)
    0xC0                     // End Collection
};

class SecurityCallbacks final : public BLESecurityCallbacks {
 public:
  bool onSecurityRequest() override { return true; }
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    Serial.printf("BLE pairing %s\n", result.success ? "complete" : "failed");
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc* result) override {
    const bool secure =
        result != nullptr && result->sec_state.encrypted && result->sec_state.bonded;
    Serial.printf("BLE pairing %s\n", secure ? "complete" : "failed");
  }
#endif
};

#if defined(CODEX_STOPWATCH_USB_MIC) && defined(CONFIG_BLUEDROID_ENABLED)
void clearIncompatibleBondsOnce() {
  Preferences preferences;
  if (!preferences.begin("codex-mic", false)) return;
  constexpr uint8_t kGattRevision = 1;
  if (preferences.getUChar("gatt-rev", 0) >= kGattRevision) {
    preferences.end();
    return;
  }

  int count = esp_ble_get_bond_device_num();
  int removed = 0;
  if (count > 0) {
    auto* devices = new esp_ble_bond_dev_t[count];
    int listed = count;
    if (esp_ble_get_bond_device_list(&listed, devices) == ESP_OK) {
      for (int index = 0; index < listed; ++index) {
        if (esp_ble_remove_bond_device(devices[index].bd_addr) == ESP_OK) {
          ++removed;
        }
      }
    }
    delete[] devices;
  }
  preferences.putUChar("gatt-rev", kGattRevision);
  preferences.end();
  Serial.printf("BLE bond migration complete removed=%d\n", removed);
}
#endif

}  // namespace

class CodexMicroBle::ServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit ServerCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  // Arduino-ESP32 invokes both overloads for every Bluedroid event. Keep the
  // parameterless hooks quiet and consume only the conn_id-bearing callback.
  void onConnect(BLEServer*) override { Serial.println("DBG onConnect()"); }
  void onDisconnect(BLEServer*) override { Serial.println("DBG onDisconnect()"); }

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    Serial.printf("DBG onConnect(param=%p)\n", static_cast<void*>(param));
    if (param != nullptr) {
      owner_.onConnectionEvent(true, param->connect.conn_id);
      // CODEX_STOPWATCH_THIRD_PARTY: Windows (Intel BT) does not re-subscribe
      // the input-report CCCD on reconnect, so notify() events are silently
      // ignored by the host (esp-idf IDFGH-14581 / PR #15337). Force the
      // local CCCD value to "notifications enabled" so Windows accepts the
      // input reports and the ChatGPT app receives key presses.
      if (owner_.input_ != nullptr) {
        BLEDescriptor* cccd =
            owner_.input_->getDescriptorByUUID(BLEUUID(static_cast<uint16_t>(0x2902)));
        if (cccd != nullptr) {
          const uint8_t enabled[2] = {0x01, 0x00};
          cccd->setValue(const_cast<uint8_t*>(enabled), sizeof(enabled));
          Serial.println("DBG input CCCD forced to notifications");
        }
      }
      // Windows (Intel BT) dropped the link seconds after pairing when relying
      // on default connection parameters. Request a stable profile: 15-30ms
      // interval, no slave latency, and a generous 10s supervision timeout so
      // brief radio hiccups do not kill the HID session.
      esp_ble_conn_update_params_t conn{};
      memcpy(conn.bda, param->connect.remote_bda, sizeof(conn.bda));
      conn.min_int = 0x0C;   // 15 ms
      conn.max_int = 0x18;   // 30 ms
      conn.latency = 0;
      conn.timeout = 1000;   // 10 s (units of 10 ms)
      esp_err_t rc = esp_ble_gap_update_conn_params(&conn);
      Serial.printf("DBG update_conn_params rc=%d\n", static_cast<int>(rc));
    }
    // macOS owns the first connection for HOGP. Keep advertising so the
    // companion can establish a second app-level connection to the private
    // quota service instead of being locked out by the HID link.
    BLEDevice::startAdvertising();
  }

  void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    Serial.printf("DBG onDisconnect(param=%p)\n", static_cast<void*>(param));
    if (param != nullptr) {
      owner_.onConnectionEvent(false, param->disconnect.conn_id);
    }
    BLEDevice::startAdvertising();
  }
#endif

 private:
  CodexMicroBle& owner_;
};

class CodexMicroBle::OutputCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit OutputCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  void onWrite(BLECharacteristic* characteristic,
               esp_ble_gatts_cb_param_t* param) override {
    if (param == nullptr) return;
    const auto value = characteristic->getValue();
    owner_.onOutput(reinterpret_cast<const uint8_t*>(value.c_str()),
                    value.length(), param->write.conn_id,
                    param->write.bda);
  }

 private:
  CodexMicroBle& owner_;
};

class CodexMicroBle::QuotaCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit QuotaCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  void onWrite(BLECharacteristic* characteristic,
               esp_ble_gatts_cb_param_t* param) override {
    if (param == nullptr) return;
    const auto value = characteristic->getValue();
    owner_.onQuotaWrite(reinterpret_cast<const uint8_t*>(value.c_str()),
                        value.length(), param->write.need_rsp,
                        param->write.bda);
  }

 private:
  CodexMicroBle& owner_;
};

void CodexMicroBle::begin() {
  stateMutex_ = xSemaphoreCreateMutex();
  outputQueue_ = xQueueCreate(16, sizeof(PendingOutputReport));
  if (outputQueue_ == nullptr) {
    Serial.println("BLE output queue allocation failed");
  }
  connectionEventQueue_ = xQueueCreate(16, sizeof(PendingConnectionEvent));
  if (connectionEventQueue_ == nullptr) {
    Serial.println("BLE connection event queue allocation failed");
  }
  quotaWriteQueue_ = xQueueCreate(4, sizeof(PendingQuotaWrite));
  if (quotaWriteQueue_ == nullptr) {
    Serial.println("BLE quota write queue allocation failed");
  }

  BLEDevice::init(kDeviceName);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

#if defined(CODEX_STOPWATCH_USB_MIC) && defined(CONFIG_BLUEDROID_ENABLED)
  // Arduino-ESP32 3.x and 2.x persist different Bluedroid GATT metadata. Clear
  // the old bond exactly once when installing the optional USB-mic image, so
  // macOS negotiates the compact table below instead of retrying stale keys.
  clearIncompatibleBondsOnce();
#endif

  auto* security = new BLESecurity();
  security->setCapability(ESP_IO_CAP_NONE);
  security->setAuthenticationMode(ESP_LE_AUTH_BOND);

  server_ = BLEDevice::createServer();
  BLEServer* server = server_;
  server->setCallbacks(new ServerCallbacks(*this));

#if defined(CODEX_STOPWATCH_COMPACT_GATT)
  // CODEX_STOPWATCH_THIRD_PARTY: this manual table is the Windows-friendly
  // HID service. BLEHIDDevice's 40-handle 0x1812 service fails to register on
  // Arduino-ESP32 3.x (the optional usb-mic image), so we build the HID
  // service by hand with a small handle count: same characteristics HOGP
  // requires (Report Map, Input+Output Report, HID Info, Control Point,
  // Protocol Mode, Report Reference, CCCD), but under the STANDARD 0x1812
  // UUID so Windows can pair the watch as an HID device and the ChatGPT app's
  // node-hid integration sees VID 303A/PID 8360 usage FF00.
  BLEService* deviceInfo =
      server->createService(BLEUUID(static_cast<uint16_t>(0x180A)), 8);
  // CODEX_STOPWATCH_THIRD_PARTY: keep numHandles small. A large value (20)
  // made esp_ble_gatts_create_service fail silently, leaving m_handle NULL so
  // start() bailed and the service never registered.
  hidService_ =
      server->createService(BLEUUID(static_cast<uint16_t>(0x1812)), 15);
  BLEService* batteryService =
      server->createService(BLEUUID(static_cast<uint16_t>(0x180F)), 6);

  BLECharacteristic* manufacturer = deviceInfo->createCharacteristic(
      static_cast<uint16_t>(0x2A29), BLECharacteristic::PROPERTY_READ);
  manufacturer->setValue(kManufacturer);
  BLECharacteristic* pnp = deviceInfo->createCharacteristic(
      static_cast<uint16_t>(0x2A50), BLECharacteristic::PROPERTY_READ);
  const uint8_t pnpValue[] = {
      0x02,
      static_cast<uint8_t>(kVendorId & 0xFF),
      static_cast<uint8_t>(kVendorId >> 8),
      static_cast<uint8_t>(kProductId & 0xFF),
      static_cast<uint8_t>(kProductId >> 8),
      0x01,
      0x01,
  };
  pnp->setValue(const_cast<uint8_t*>(pnpValue), sizeof(pnpValue));

  BLECharacteristic* hidInfo = hidService_->createCharacteristic(
      static_cast<uint16_t>(0x2A4A), BLECharacteristic::PROPERTY_READ);
  const uint8_t hidInfoValue[] = {0x11, 0x01, 0x00, 0x01};
  hidInfo->setValue(const_cast<uint8_t*>(hidInfoValue), sizeof(hidInfoValue));
  BLECharacteristic* reportMap = hidService_->createCharacteristic(
      static_cast<uint16_t>(0x2A4B), BLECharacteristic::PROPERTY_READ);
  reportMap->setValue(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));
  hidService_->createCharacteristic(
      static_cast<uint16_t>(0x2A4C), BLECharacteristic::PROPERTY_WRITE_NR);
  BLECharacteristic* protocolMode = hidService_->createCharacteristic(
      static_cast<uint16_t>(0x2A4E),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE_NR);
  const uint8_t reportProtocol = 0x01;
  protocolMode->setValue(const_cast<uint8_t*>(&reportProtocol), 1);

  input_ = hidService_->createCharacteristic(
      static_cast<uint16_t>(0x2A4D),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  // CODEX_STOPWATCH_THIRD_PARTY: Windows bleak bridge clients subscribe to
  // this input report without a macOS-style encrypted bond. The payload is
  // the same non-sensitive vendor JSON used over the HID channel.
  input_->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  auto* inputReference =
      new BLEDescriptor(BLEUUID(static_cast<uint16_t>(0x2908)));
  const uint8_t inputReferenceValue[] = {kReportId, 0x01};
  inputReference->setValue(const_cast<uint8_t*>(inputReferenceValue),
                           sizeof(inputReferenceValue));
  inputReference->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  input_->addDescriptor(inputReference);
  auto* inputCccd = new BLE2902();
  inputCccd->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  input_->addDescriptor(inputCccd);

  BLECharacteristic* output = hidService_->createCharacteristic(
      static_cast<uint16_t>(0x2A4D),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);
  // CODEX_STOPWATCH_THIRD_PARTY: plain permissions, not ENCRYPTED. Windows
  // hidbthle performs HID control-point/protocol writes before (or without)
  // a full SMP encryption handshake on reconnect; an ENCRYPTED gate made the
  // HID driver drop the link right after pairing. The payload is non-sensitive
  // vendor JSON, so plain access is acceptable.
  output->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  auto* outputReference =
      new BLEDescriptor(BLEUUID(static_cast<uint16_t>(0x2908)));
  const uint8_t outputReferenceValue[] = {kReportId, 0x02};
  outputReference->setValue(const_cast<uint8_t*>(outputReferenceValue),
                            sizeof(outputReferenceValue));
  outputReference->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  output->addDescriptor(outputReference);
  output->setCallbacks(new OutputCallbacks(*this));

  battery_ = batteryService->createCharacteristic(
      static_cast<uint16_t>(0x2A19),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  auto* batteryFormat = new BLE2904();
  batteryFormat->setFormat(BLE2904::FORMAT_UINT8);
  batteryFormat->setNamespace(1);
  batteryFormat->setUnit(0x27AD);
  battery_->addDescriptor(batteryFormat);
  auto* batteryCccd = new BLE2902();
  batteryCccd->setNotifications(true);
  batteryCccd->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  battery_->addDescriptor(batteryCccd);
  battery_->setValue(&batteryPercentage_, 1);

  // CODEX_STOPWATCH_THIRD_PARTY: keep the service starts API-compatible with
  // both Arduino-ESP32 2.x (void returns) and 3.x (bool returns). We ignore
  // the return value; the important part is that the 0x1812 HID service now
  // actually registers (BLEHIDDevice's 40-handle service failed silently).
  deviceInfo->start();
  hidService_->start();
  batteryService->start();
  Serial.printf("BLE compact services device_info hid battery started "
                "(hid_handle=%u)\n",
                static_cast<unsigned>(hidService_->getHandle()));
#else
  hid_ = new BLEHIDDevice(server);
  hid_->manufacturer()->setValue(kManufacturer);
  // Low release bits mark the transport as wireless in the desktop bridge.
  // Arduino-ESP32 2.x serializes these fields big-endian, while the BLE PnP
  // characteristic is little-endian. Pre-swap so macOS enumerates 303A:8360.
  hid_->pnp(0x02, swapBytes(kVendorId), swapBytes(kProductId), swapBytes(0x0101));
  hid_->hidInfo(0x00, 0x01);
  hid_->reportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));

  input_ = hid_->inputReport(kReportId);
  BLECharacteristic* output = hid_->outputReport(kReportId);
  output->setCallbacks(new OutputCallbacks(*this));
  hid_->startServices();
  hid_->setBatteryLevel(batteryPercentage_);
  hidService_ = hid_->hidService();
  // CODEX_STOPWATCH_THIRD_PARTY: confirm the real 0x1812 service registered
  // (handle != 0). Without it Windows never offers an HID pairing and the
  // ChatGPT app's node-hid integration cannot see the watch.
  Serial.printf("BLE HID 0x1812 registered handle=%u\n",
                static_cast<unsigned>(hidService_->getHandle()));
#endif

  // This private GATT service is intentionally separate from the emulated
  // Codex Micro HID protocol. A trusted Mac companion writes rate-limit
  // snapshots here; ChatGPT Desktop continues to own all control actions.
#if defined(CODEX_STOPWATCH_COMPACT_GATT)
  BLEService* quotaService =
      server->createService(BLEUUID(kQuotaServiceUuid));
#else
  BLEService* quotaService = server->createService(kQuotaServiceUuid);
#endif
  BLECharacteristic* quotaWrite = quotaService->createCharacteristic(
      kQuotaWriteUuid,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  // Requiring link encryption makes the first companion write establish the
  // same trusted BLE bond used by the HID reports. On macOS this also avoids a
  // separate, fragile name-based pairing step in System Settings.
  // CODEX_STOPWATCH_THIRD_PARTY: Windows bleak clients (our companion) do not
  // perform the macOS-style encrypted bond; the quota payload is a tiny,
  // non-sensitive percentage + countdown, so plain writes are acceptable.
  quotaWrite->setAccessPermissions(ESP_GATT_PERM_WRITE);
  quotaWrite->setCallbacks(new QuotaCallbacks(*this));
#if defined(CODEX_STOPWATCH_COMPACT_GATT)
  quotaService->start();
  Serial.printf("Quota GATT service started\n");
#else
  quotaService->start();
#endif

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(GENERIC_HID);
  advertising->addServiceUUID(hidService_->getUUID());
  advertising->addServiceUUID(kQuotaServiceUuid);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.printf(
      "BLE vendor HID ready VID=%04X PID=%04X usage=FF00 report=%u\n",
      kVendorId, kProductId, kReportId);
  Serial.printf("Quota GATT ready service=%s write=%s\n", kQuotaServiceUuid,
                kQuotaWriteUuid);
}

void CodexMicroBle::poll() {
  processConnectionEvents();
  reconcileConnectionSet();
  processQuotaWrites();
#if defined(CODEX_STOPWATCH_USB_MIC)
  processPendingBootloaderRestart();
#endif

  if (outputQueue_ == nullptr) return;

  PendingOutputReport report;
  while (xQueueReceive(outputQueue_, &report, 0) == pdTRUE) {
    if (report.length == 0) {
      rpcBuffer_.clear();
      continue;
    }
    processOutput(report.data.data(), report.length, report.connectionId,
                  report.peerAddress);
  }
}

void CodexMicroBle::setBattery(uint8_t percentage, bool charging) {
  batteryPercentage_ = constrain(percentage, 0, 100);
  charging_ = charging;
  if (battery_ != nullptr) {
    battery_->setValue(&batteryPercentage_, 1);
    if (connected()) battery_->notify();
  } else if (hid_ != nullptr && connected()) {
    hid_->setBatteryLevel(batteryPercentage_);
  }
}

void CodexMicroBle::sendKey(const char* key, uint8_t action, int8_t agent) {
  StaticJsonDocument<192> message;
  message["method"] = "v.oai.hid";
  JsonObject params = message.createNestedObject("params");
  params["k"] = key;
  params["act"] = action;
  if (agent >= 0) {
    params["ag"] = agent;
  }

  String json;
  serializeJson(message, json);
  sendJson(json);
  Serial.printf("HID key=%s action=%u\n", key, action);
}

void CodexMicroBle::sendJoystick(float angle, float distance) {
  StaticJsonDocument<160> message;
  message["method"] = "v.oai.rad";
  JsonObject params = message.createNestedObject("params");
  params["a"] = angle;
  params["d"] = distance;

  String json;
  serializeJson(message, json);
  sendJson(json);
}

bool CodexMicroBle::connected() {
  if (stateMutex_ != nullptr) {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    const bool result = state_.connected;
    xSemaphoreGive(stateMutex_);
    if (result) {
      return true;
    }
  }
  if (server_ != nullptr && server_->getConnectedCount() > 0) {
    return true;
  }
  // CODEX_STOPWATCH_THIRD_PARTY: on this build the server's own counters
  // (state_.connected / getConnectedCount) can report 0 while a host is
  // actively writing (observed with the Windows HID driver: WRITE events
  // arrive yet sendJson kept being dropped as "not connected"). Fall back to
  // the actual connected-peer list, which is not derived from the
  // connect/disconnect accounting.
  if (server_ != nullptr) {
    const auto peers = server_->getPeerDevices(false);
    if (!peers.empty()) {
      return true;
    }
  }
  return false;
}

CodexMicroState CodexMicroBle::snapshot() {
  CodexMicroState copy;
  if (stateMutex_ == nullptr) {
    return copy;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  copy = state_;
  state_.dirty = false;
  xSemaphoreGive(stateMutex_);
  return copy;
}

void CodexMicroBle::onConnectionEvent(bool connected, uint16_t id) {
  if (connectionEventQueue_ == nullptr) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }
  PendingConnectionEvent event;
  event.id = id;
  event.connected = connected;
  if (xQueueSend(connectionEventQueue_, &event, 0) != pdTRUE) {
    connectionEventLost_.store(true, std::memory_order_release);
  }
}

void CodexMicroBle::applyConnectionEvent(const PendingConnectionEvent& event) {
  const connection_health::ConnectionSet::Transition transition =
      connections_.apply(event.connected, event.id);
  if (transition.overflow) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }
  if (!transition.changed || stateMutex_ == nullptr) return;

  const uint8_t count = static_cast<uint8_t>(connections_.count());

  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool hostDisconnected =
      !event.connected && hostRpcConnectionValid_ &&
      hostRpcConnectionId_ == event.id;
  if (transition.becameConnected) {
    ++state_.connectionEpoch;
    state_.lastHostRpcAtMs = 0;
    state_.hostRpcObserved = false;
    clearHostRpcIdentity();
  } else if (transition.becameDisconnected || hostDisconnected) {
    state_.lastHostRpcAtMs = 0;
    state_.hostRpcObserved = false;
    clearHostRpcIdentity();
  }
  state_.connected = count > 0;
  state_.dirty = true;
  const uint32_t connectionEpoch = state_.connectionEpoch;
  xSemaphoreGive(stateMutex_);

  if (transition.becameConnected || transition.becameDisconnected ||
      (!event.connected && rpcBufferConnectionValid_ &&
       rpcBufferConnectionId_ == event.id)) {
    rpcBuffer_.clear();
    rpcBufferConnectionValid_ = false;
  }
  Serial.printf("BLE host event=%s id=%u count=%u epoch=%lu\n",
                event.connected ? "connected" : "disconnected", event.id,
                count, static_cast<unsigned long>(connectionEpoch));
}

void CodexMicroBle::processConnectionEvents() {
  if (connectionEventQueue_ == nullptr) return;
  PendingConnectionEvent event;
  while (xQueueReceive(connectionEventQueue_, &event, 0) == pdTRUE) {
    applyConnectionEvent(event);
  }
}

void CodexMicroBle::reconcileConnectionSet() {
  if (server_ == nullptr || stateMutex_ == nullptr) return;
  const bool eventLost =
      connectionEventLost_.exchange(false, std::memory_order_acq_rel);
  if (!eventLost) return;
  connection_health::ConnectionSet observed;
  bool overflow = false;
  const auto peers = server_->getPeerDevices(false);
  for (const auto& peer : peers) {
    if (observed.apply(true, peer.first).overflow) overflow = true;
  }
  if (overflow) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }

  // At least one ordered event was lost. Fail closed even if final membership
  // happens to match: an unseen empty->connected cycle must never inherit the
  // old session's CODEX LIVE state.
  connections_ = observed;
  const uint8_t count = static_cast<uint8_t>(connections_.count());
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (count > 0) {
    ++state_.connectionEpoch;
  }
  state_.connected = count > 0;
  state_.lastHostRpcAtMs = 0;
  state_.hostRpcObserved = false;
  clearHostRpcIdentity();
  state_.dirty = true;
  const uint32_t epoch = state_.connectionEpoch;
  xSemaphoreGive(stateMutex_);
  rpcBuffer_.clear();
  rpcBufferConnectionValid_ = false;
  Serial.printf("BLE connections reconciled count=%u epoch=%lu\n", count,
                static_cast<unsigned long>(epoch));
}

void CodexMicroBle::clearHostRpcIdentity() {
  hostRpcConnectionValid_ = false;
#if defined(CODEX_STOPWATCH_USB_MIC)
  hostRpcPeerAddress_.fill(0);
  hostRpcPeerEpoch_ = 0;
  hostRpcPeerValid_ = false;
#endif
}

void CodexMicroBle::noteHostRpcActivity(
    uint16_t connectionId, const PeerAddress& peerAddress) {
  if (stateMutex_ == nullptr || !connections_.contains(connectionId)) return;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (!state_.connected) {
    xSemaphoreGive(stateMutex_);
    return;
  }
  const bool promoted = !state_.hostRpcObserved;
  state_.hostRpcObserved = true;
  state_.lastHostRpcAtMs = millis();
  hostRpcConnectionId_ = connectionId;
  hostRpcConnectionValid_ = true;
#if defined(CODEX_STOPWATCH_USB_MIC)
  hostRpcPeerAddress_ = peerAddress;
  hostRpcPeerEpoch_ = state_.connectionEpoch;
  hostRpcPeerValid_ = true;
#endif
  if (promoted) state_.dirty = true;
  xSemaphoreGive(stateMutex_);
}

void CodexMicroBle::onOutput(const uint8_t* data, size_t length,
                             uint16_t connectionId,
                             const uint8_t* peerAddress) {
  if (data == nullptr || length == 0 || peerAddress == nullptr ||
      outputQueue_ == nullptr) {
    return;
  }

  PendingOutputReport report;
  report.length = static_cast<uint8_t>(min<size_t>(length, report.data.size()));
  report.connectionId = connectionId;
  memcpy(report.peerAddress.data(), peerAddress, report.peerAddress.size());
  memcpy(report.data.data(), data, report.length);
  xQueueSend(outputQueue_, &report, 0);
}

void CodexMicroBle::processOutput(const uint8_t* data, size_t length,
                                  uint16_t connectionId,
                                  const PeerAddress& peerAddress) {
  if (data == nullptr || length < 2 || !connections_.contains(connectionId)) {
    return;
  }

  if (rpcBufferConnectionValid_ &&
      rpcBufferConnectionId_ != connectionId) {
    rpcBuffer_.clear();
    rpcBufferConnectionValid_ = false;
  }

  // HOGP normally strips the report ID. Accept an included ID as well so the
  // transport remains compatible with hosts that forward the raw report.
  size_t offset = (length >= 3 && data[0] == kReportId) ? 1 : 0;
  if (length < offset + 2 || data[offset] != 2) {
    return;
  }

  const size_t payloadLength = min<size_t>(data[offset + 1], kPayloadSize);
  if (length < offset + 2 + payloadLength) {
    return;
  }
  const char* payload = reinterpret_cast<const char*>(data + offset + 2);
  constexpr char kTopLevelPrefix[] = "{\"method\"";
  const bool startsTopLevel =
      payloadLength >= sizeof(kTopLevelPrefix) - 1 &&
      memcmp(payload, kTopLevelPrefix, sizeof(kTopLevelPrefix) - 1) == 0;
  if (startsTopLevel && !rpcBuffer_.isEmpty()) {
    // A new top-level object means a previous fragmented write was dropped.
    // Resynchronize immediately instead of poisoning the next request.
    rpcBuffer_.clear();
  }
  if (rpcBuffer_.isEmpty()) {
    size_t jsonStart = 0;
    while (jsonStart < payloadLength && payload[jsonStart] != '{') {
      ++jsonStart;
    }
    if (jsonStart == payloadLength) {
      return;
    }
    rpcBuffer_.concat(payload + jsonStart, payloadLength - jsonStart);
    rpcBufferConnectionId_ = connectionId;
    rpcBufferConnectionValid_ = true;
  } else {
    rpcBuffer_.concat(payload, payloadLength);
  }

  DynamicJsonDocument request(4096);
  const DeserializationError error = deserializeJson(request, rpcBuffer_);
  if (error == DeserializationError::IncompleteInput) {
    return;
  }
  if (error) {
    Serial.printf("RPC parse error: %s\n", error.c_str());
    rpcBuffer_.clear();
    return;
  }

  if (handleRpc(request)) {
    noteHostRpcActivity(connectionId, peerAddress);
  }
  rpcBuffer_.clear();
  rpcBufferConnectionValid_ = false;
}

void CodexMicroBle::onQuotaWrite(const uint8_t* data, size_t length,
                                 bool responseExpected,
                                 const uint8_t* peerAddress) {
  if (data == nullptr || length == 0 || length > 512 ||
      peerAddress == nullptr || quotaWriteQueue_ == nullptr) {
    Serial.println("Quota update rejected: invalid length");
    return;
  }

  PendingQuotaWrite write;
  write.length = static_cast<uint16_t>(length);
  write.responseExpected = responseExpected;
  memcpy(write.peerAddress.data(), peerAddress, write.peerAddress.size());
  memcpy(write.data.data(), data, length);
  if (xQueueSend(quotaWriteQueue_, &write, 0) != pdTRUE) {
    Serial.println("Quota update rejected: queue full");
  }
}

void CodexMicroBle::processQuotaWrites() {
  if (quotaWriteQueue_ == nullptr) return;
  PendingQuotaWrite write;
  while (xQueueReceive(quotaWriteQueue_, &write, 0) == pdTRUE) {
    processQuotaWrite(write.data.data(), write.length, write.responseExpected,
                      write.peerAddress);
  }
}

void CodexMicroBle::processQuotaWrite(const uint8_t* data, size_t length,
                                      bool responseExpected,
                                      const PeerAddress& peerAddress) {
  if (data == nullptr || length == 0 || length > 512) {
    Serial.println("Quota update rejected: invalid length");
    return;
  }

  DynamicJsonDocument update(768);
  const DeserializationError error = deserializeJson(update, data, length);
  if (error || !update.is<JsonObject>()) {
    Serial.printf("Quota update rejected: %s\n", error.c_str());
    return;
  }

#if defined(CODEX_STOPWATCH_USB_MIC)
  const JsonObjectConst request = update.as<JsonObjectConst>();
  if (request.containsKey("op")) {
    if (!isBootloaderRequest(data, length)) {
      Serial.println("Bootloader request rejected: invalid command");
      return;
    }
    if (!responseExpected) {
      Serial.println("Bootloader request rejected: ATT response required");
      return;
    }
    if (!isBootloaderPeerAuthorized(peerAddress)) {
      Serial.println("Bootloader request rejected: HID host mismatch");
      return;
    }
    if (!usbPowerPresent()) {
      Serial.println("Bootloader request rejected: USB power not present");
      return;
    }
    if (!bootloaderRestartPending_) {
      bootloaderRestartPending_ = true;
      bootloaderRestartAtMs_ = millis() + kBootloaderRestartDelayMs;
      Serial.println("Bootloader request accepted; restart pending");
    }
    return;
  }
#endif

  quota_payload::Snapshot snapshot;
  if (!quota_payload::parse(update.as<JsonObjectConst>(), snapshot)) {
    Serial.println("Quota update rejected: invalid fields");
    return;
  }

  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  state_.quota.remainingPercent = snapshot.remainingPercent;
  state_.quota.resetInSeconds = snapshot.resetInSeconds;
  state_.quota.receivedAtMs = millis();
  state_.quota.available = true;
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);

  Serial.printf("Quota update remaining=%.1f reset=%lus\n",
                snapshot.remainingPercent,
                static_cast<unsigned long>(snapshot.resetInSeconds));
}

#if defined(CODEX_STOPWATCH_USB_MIC)
bool CodexMicroBle::isBootloaderRequest(const uint8_t* data,
                                        size_t length) const {
  constexpr size_t kExpectedLength = sizeof(kBootloaderRequest) - 1;
  return data != nullptr && length == kExpectedLength &&
         memcmp(data, kBootloaderRequest, kExpectedLength) == 0;
}

bool CodexMicroBle::isBootloaderPeerAuthorized(
    const PeerAddress& peerAddress) const {
  if (stateMutex_ == nullptr) return false;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool authorized =
      hostRpcPeerValid_ && hostRpcConnectionValid_ && state_.hostRpcObserved &&
      hostRpcPeerEpoch_ == state_.connectionEpoch &&
      hostRpcPeerAddress_ == peerAddress;
  xSemaphoreGive(stateMutex_);
  return authorized;
}

bool CodexMicroBle::usbPowerPresent() const {
  return M5.Power.getVBUSVoltage() >= kBootloaderMinimumVbusMv;
}

void CodexMicroBle::processPendingBootloaderRestart() {
  if (!bootloaderRestartPending_ ||
      static_cast<int32_t>(millis() - bootloaderRestartAtMs_) < 0) {
    return;
  }
  bootloaderRestartPending_ = false;
  if (!usbPowerPresent()) {
    Serial.println("Bootloader restart cancelled: USB power removed");
    return;
  }

  Serial.println("BOOTLOADER_RESTART");
  Serial.flush();
  usb_persist_restart(RESTART_BOOTLOADER);
}
#endif

bool CodexMicroBle::handleRpc(const JsonDocument& request) {
  const JsonObjectConst requestObject = request.as<JsonObjectConst>();
  const char* method = requestObject["method"] | "";
  const JsonVariantConst id = requestObject["id"];
  const JsonVariantConst params = requestObject["params"];
  const host_rpc::Method supportedMethod = host_rpc::classify(requestObject);
  Serial.printf("RPC method=%s\n", method);

  if (supportedMethod == host_rpc::Method::SystemVersion) {
    StaticJsonDocument<128> resultDoc;
    resultDoc["version"] = kFirmwareVersion;
    sendResult(id, resultDoc.as<JsonVariantConst>());
    return true;
  }

  if (supportedMethod == host_rpc::Method::DeviceStatus) {
    StaticJsonDocument<256> resultDoc;
    resultDoc["version"] = kFirmwareVersion;
    resultDoc["profile_index"] = 0;
    resultDoc["layer_index"] = 1;
    resultDoc["battery"] = batteryPercentage_;
    resultDoc["is_charging"] = charging_;
    sendResult(id, resultDoc.as<JsonVariantConst>());
    return true;
  }

  if (supportedMethod == host_rpc::Method::ThreadStatus) {
    updateThreadLighting(params.as<JsonArrayConst>());
    sendSuccess(id);
    return true;
  }

  if (supportedMethod == host_rpc::Method::RgbConfig) {
    sendSuccess(id);
    return true;
  }

  if (supportedMethod == host_rpc::Method::LightsPreview ||
      supportedMethod == host_rpc::Method::HostFocusedApp) {
    sendSuccess(id);
    return true;
  }

  StaticJsonDocument<192> response;
  response["id"] = id;
  JsonObject error = response.createNestedObject("error");
  error["code"] = -32601;
  error["message"] = "Method not found";
  String json;
  serializeJson(response, json);
  sendJson(json);
  return false;
}

void CodexMicroBle::sendResult(JsonVariantConst id, JsonVariantConst result) {
  DynamicJsonDocument response(512);
  response["id"] = id;
  response["result"] = result;
  String json;
  serializeJson(response, json);
  sendJson(json);
}

void CodexMicroBle::sendSuccess(JsonVariantConst id) {
  StaticJsonDocument<96> resultDoc;
  resultDoc["ok"] = true;
  sendResult(id, resultDoc.as<JsonVariantConst>());
}

void CodexMicroBle::sendJson(const String& json) {
  if (input_ == nullptr) {
    Serial.println("DBG sendJson input_=null");
    return;
  }
  if (!connected()) {
    Serial.println("DBG sendJson dropped: not connected");
    return;
  }

  String framed = json;
  framed += '\n';
  size_t offset = 0;
  while (offset < framed.length()) {
    const size_t chunk = min<size_t>(kPayloadSize, framed.length() - offset);
    uint8_t report[kReportBodySize] = {};
    report[0] = 2;
    report[1] = chunk;
    memcpy(report + 2, framed.c_str() + offset, chunk);
    input_->setValue(report, sizeof(report));
    input_->notify();
    Serial.printf("DBG notify len=%d sent\n", static_cast<int>(sizeof(report)));
    offset += chunk;
    delay(4);
  }
}

void CodexMicroBle::updateThreadLighting(JsonArrayConst values) {
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  for (JsonObjectConst value : values) {
    const int id = value["id"] | -1;
    if (id < 0 || id >= static_cast<int>(state_.threads.size())) {
      continue;
    }
    ThreadLight& light = state_.threads[id];
    light.color = value["c"] | light.color;
    light.brightness = value["b"] | light.brightness;
    light.effect = value["e"] | light.effect;
    light.speed = value["s"] | light.speed;
  }
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);
}
