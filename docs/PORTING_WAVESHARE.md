# Porting Notes: Waveshare ESP32-S3-Touch-AMOLED-1.75C (Windows)

This document records how the C152-targeted `codex-micro-stopwatch` firmware was
adapted to run on the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** development
board (466×466 CO5300 QSPI AMOLED, CST9217 touch, AXP2101 PMIC, ES8311 codec,
PWR/BOOT side buttons, no M5PM1/M5IOE1), including a **Windows host-side
quota companion** so the whole device works without a Mac.

> Status: fully functional on real hardware (Windows). Display/touch/BLE/HID,
> ChatGPT Desktop pairing and controls, real Codex quota via the direct
> `chatgpt.com/backend-api/wham/usage` endpoint written to the watch over BLE,
> AXP2101 battery, ES8311 chime, physical BOOT/PWR buttons, desk sleep.

## Host-side Windows companion

`companion-win/` is a Python (bleak) port of the upstream Swift companion:

| File | Purpose |
| --- | --- |
| `companion-win/src/codex_watch_all.py` | All-in-one: quota read + BLE GATT write every 120s (direct `wham/usage` call, no app-server) |
| `companion-win/src/codex_watch_bridge.py` | Subscribe to the watch HID channel and drive Codex turns |
| `companion-win/src/codex_watch_companion.py` | App-server JSON-RPC client + BLE GATT quota writer (optional path) |
| `companion-win/setup_win_companion.ps1` | One-shot setup: find Python, install bleak, check codex login, run companion |
| `companion-win/install_startup_task.ps1` | Register a logon scheduled task for automatic quota refresh |

Quota read notes (`codex_watch_all.py`):
- Reads the local Codex `auth.json` (`CODEX_HOME`, or `~/.codex`) access token
  and calls `https://chatgpt.com/backend-api/wham/usage` directly. The
  `codex app-server` path is optional and was unreliable on this machine.
- Writes `{"remaining_percent": n, "reset_in_seconds": n}` (<= 512 B) to the
  watch over BLE GATT service `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01/02`.
- The companion connects briefly every 120s and disconnects, so the Windows
  HID link (one LE connection) keeps voice Push-to-talk working in between.

## Hardware differences vs C152

| Component | C152 (upstream) | Waveshare 1.75C |
| --- | --- | --- |
| Display | CO5300 466×466 QSPI | CO5300 466×466 QSPI (same panel, different pins) |
| Display pins | CS=39, SCLK=40, IO0-3=41/42/45/46 | CS=12, PCLK=38, D0-3=4/5/6/7 |
| Touch | CST816S (0x15) on GPIO47/48 | CST9217 (0x5A) on GPIO14/15, INT=11, RST=2 |
| PMIC | M5PM1 (0x6E) + M5IOE1 (0x4F) | AXP2101 (0x34) |
| Audio | ES8311 via M5Unified C152 profile | ES8311 (0x18), MCLK=16/BCLK=9/WS=45/DOUT=8, PA=GPIO46 |
| Buttons | BtnA=GPIO2, BtnB=GPIO1 | PWR (AXP2101 PEK) + BOOT (GPIO0) side buttons; GPIO1/2 are LCD/TP resets |
| Motor | vibration | none |

## What was changed

### `platformio.win.ini` (Windows build override)
- `lib_deps` use **zip archives pinned to the same commits** instead of
  `git+https` (the sandbox blocks `git clone --recursive`; zip content is
  identical).
- `extra_scripts` adds `pre:scripts/patch_m5gfx_waveshare.py` next to the
  existing `patch_m5gfx_amoled_sleep.py`.

### `scripts/patch_m5gfx_waveshare.py` (reproducible)
Idempotent pre-build patch for the pinned M5GFX:
1. **Waveshare autodetect branch**: probes CST92xx touch at 0x5A on
   GPIO14/15 *before* M5Stack probes (which would misdetect the board as
   M5PowerHub), then configures the CO5300 panel on Waveshare pins
   (CS=12, SCLK=38, IO0-3=4/5/6/7) plus `Touch_CST226` (0x5A, SDA=15/SCL=14,
   RST=2) with **`pin_int = GPIO_NUM_NC`** (the INT pin on this board makes
   M5GFX bail out of `getTouchRaw`), and sets `board_M5StopWatch`.
2. **Touch_CST226 chip-id check**: accepts CST92xx (0x92xx) in addition to
   CST226 (0xa8), so the CST9217 passes init.

### `src/Axp2101Power.h`
Minimal AXP2101 driver over the already-initialized touch I2C bus
(`lgfx::i2c` on I2C_NUM_1) — never installs a second I2C master on GPIO14/15.
Provides battery SOC/voltage, charging, VBUS, soft power-off, PEK press.

### `src/BoardAudio.h`
Manual ES8311 configuration for Waveshare pins + PA (GPIO46) control.
`main.cpp` starts M5Unified with `config.internal_spk = false` because the
C152 speaker profile would put I2S WS on GPIO15 (the touch SDA here).

### `src/main.cpp`
- **Touch coordinate fix**: the CST9217 reports ~0-232 coordinates that are
  MIRRORED (top-left maps to (232,232), bottom-right to (0,0)). The firmware
  maps `screen = (232 - raw) * 2` to the 466×466 framebuffer. Without the
  mirror, tapping A5 (top-left) landed on A2 (diagonal opposite).
- **Phantom-tap suppression**: the CST9217 can emit several press/release
  cycles for one physical tap, which used to fire the same Agent/Send key
  repeatedly (ChatGPT window kept popping). After a commit, a 600ms lockout
  swallows trailing cycles **at the same spot** (±100px); a press at a
  different spot (another agent, the center Send dial) is a real new tap and
  is allowed through immediately. Desk-noise bursts (>=4 commits/2s) suppress
  touches for 3s.
- **Physical side keys**: BOOT (GPIO0, active low) = Push to talk (`ACT10`);
  PWR (AXP2101 PEK short press) = Voice chat toggle (`ACT09`). The old
  edge-band long-press emulation is DISABLED (the panel reports phantom
  touches along the left edge on a desk).
- `setPanelRail`/`setPanelReset`: no-op without IOE1 (guard on
  `powerManagerReady`).
- `updatePowerTelemetry`: reads AXP2101 when `axpReady`; keeps C152 PM1 path
  otherwise (unused on this board).
- `enterDeskSleep`: cuts the shared rail only when `powerManagerReady` (C152);
  Waveshare degrades to brightness-0 + CO5300 Sleep In, bus kept alive, BLE
  online.
- `enterTravelPowerOff`: tries AXP2101 soft power-off when PM1 absent, keeps
  `esp_restart()` as fallback.

### `src/CodexMicroBle.cpp` (Windows BLE HID)
- Uses the stock **BLEHIDDevice** path (report ID 6, vendor FF00) so Windows
  pairs the watch as an HID device and the ChatGPT app's node-hid integration
  consumes reports natively.
- **Forced input-report CCCD subscription**: Windows (Intel BT) does not
  re-subscribe the notify CCCD on reconnect, so `notify()` events were
  silently ignored. The firmware forces the local CCCD value to 0x0001 on
  connect — this made Push-to-talk voice work on Windows.
- `esp_ble_gap_update_conn_params` requests a stable profile (15-30ms
  interval, no latency, 10s supervision) to avoid Windows link drops.

## Build / flash / verify (Windows)

```powershell
pio run -e m5stack-stopwatch            # or: pio run -c platformio.win.ini
pio run -e m5stack-stopwatch -t upload --upload-port COM5

# one-click flash (no PlatformIO needed if esptool is available):
powershell -ExecutionPolicy Bypass -File flash_codex.ps1 -Port COM5

python scripts/serial_probe_win.py COM5 --seconds 30 --expect CODEX_MICRO_STOPWATCH_READY
```

Expected serial output includes:
```
POWER AXP2101 ready=1
POWER AXP vbat=4129mV vbus=5051mV soc=100
AUDIO ES8311 ready=1
BLE vendor HID ready ...
Quota GATT ready ...
CODEX_MICRO_STOPWATCH_READY
POWER mode=dock|battery
```

## Known limitations (honest)

- PM1/IOE1 features (charging rail control, motor, hardware power button with
  PM1 semantics) are unavailable; AXP2101 covers battery/VBUS/charging
  telemetry and PEK-based soft power-off.
- No vibration motor on the Waveshare board; haptics are silent no-ops.
- Intel Bluetooth drivers older than ~22.x are known to drop connections; the
  firmware's connection-parameter request helps, but upgrading to 24.x is
  recommended for stability.
- AMOLED power-rail cutoff via AXP2101 is intentionally NOT implemented
  (rail wiring unconfirmed; wake-up re-init would be fragile).
