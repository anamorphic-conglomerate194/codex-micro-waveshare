# Project boundaries

- Target hardware is the Waveshare ESP32-S3-Touch-AMOLED-1.75C (466x466 AMOLED,
  CST9217 touch on GPIO14/15, AXP2101 PMIC, ES8311 codec, PWR/BOOT side
  buttons) running on Windows. This is a community port of the M5Stack
  StopWatch C152 Codex Micro controller; the upstream C152 hardware is NOT the
  target here.
- This is a third-party, unofficial port. It is not affiliated with OpenAI,
  Work Louder, M5Stack, or Waveshare. Do not imply official support.
- Do not claim a build, flash, pairing, button action, or quota update is
  validated until it is observed on physical Waveshare 1.75C hardware.
- Never infer an upload port. Resolve and report the exact COM port
  immediately before any flash operation.
- Keep ChatGPT authentication and rate-limit reads on the Windows companion
  (`companion-win/`). Never place account tokens in firmware or BLE payloads.
  The firmware and BLE channel carry only non-sensitive vendor JSON (keys,
  angles, quota percentage + reset countdown).
- Keep the emulated vendor HID path and the private quota GATT path separate.
- Preserve upstream MIT attribution and `NOTICE.md` when redistributing.
- The Waveshare board has no vibration motor and no M5PM1/M5IOE1; haptics are
  silent no-ops and power telemetry comes from the AXP2101 via `Axp2101Power.h`.
- The optional `usb-mic/` and `companion/` (macOS Swift) parts of the upstream
  project are kept out of this repository's published tree.

## Installation workflow for coding agents

- Read `README.md` and this file before changing the machine or the device.
- Confirm the connected target is a Waveshare ESP32-S3-Touch-AMOLED-1.75C.
- Build the `m5stack-stopwatch` environment with PlatformIO Core before
  flashing (`pio run -e m5stack-stopwatch`, or `-c platformio.win.ini` on
  Windows).
- Resolve the exact newly connected COM port immediately before an upload.
  Report it to the user and obtain confirmation before flashing.
- After flashing, verify the serial marker `CODEX_MICRO_STOPWATCH_READY` on the
  same exact port (`python scripts/serial_probe_win.py <port> --seconds 30
  --expect CODEX_MICRO_STOPWATCH_READY` on Windows).
- Windows Bluetooth: pair as "Codex Micro". If controls stop working after a
  reconnect, delete the pairing and pair again (the firmware forces the local
  input-report CCCD to notifications so Windows delivers HID input without a
  fresh subscribe).
- Quota sync is optional and handled by `companion-win/` (Python + bleak). It
  reads the local Codex `auth.json` access token and writes only the
  percentage + reset countdown to the watch. Never commit tokens, account
  identifiers, device MACs, or local paths.
- Do not claim success from a build alone. Report build, flash, pairing,
  controls, Agent status, and quota sync as separate validation layers.
