# Codex Micro for Waveshare ESP32-S3-Touch-AMOLED-1.75C (Windows)

[简体中文](README.zh-CN.md)

Turn a **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (466×466 AMOLED dev board)
into a wireless **Codex Micro**-compatible controller and Codex usage dashboard
on **Windows**. This is a community port of
[codex-micro-stopwatch](https://github.com/digitsisyph/codex-micro-stopwatch)
(M5Stack StopWatch C152) — the first Waveshare/Windows port of the project.

> Third-party, unofficial. Not affiliated with OpenAI, Work Louder, M5Stack,
> or Waveshare.

![Native 466 x 466 dashboard preview](artifacts/dashboard-preview-v2-round.png)

## What it does

- Shows Agent status (idle / thinking / complete / needs input / error).
- Shows Codex allowance remaining and the reset countdown.
- Shows battery, charging/Dock state, and whether Codex, BLE, and quota sync
  are actually healthy.
- Turns a completed Agent green and plays a soft completion chime.
- **BOOT button** (hold) = Push to talk (`ACT10`).
- **PWR button** (short press) = Voice Chat toggle (`ACT09`).
- **Center dial** = Send (`ACT12`).
- Maps full-screen up / right / down / left swipes to the four configurable
  Codex Micro analog-stick directions.
- Haptic feedback, BLE-aware desk sleep, and a long-hold Travel Mode
  power-off.

The control surface uses the Codex Micro-compatible BLE HID channel. Quota data
travels separately from a local Windows companion over a project-owned BLE GATT
service. The watch never stores an OpenAI token.

## Requirements

- **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (1.75" 466×466 AMOLED, CST9217
  touch on GPIO14/15, AXP2101 PMIC, ES8311 codec, PWR/BOOT side buttons).
- A Windows 10/11 PC with Bluetooth. Upgrading the Intel Bluetooth driver to
  24.x is strongly recommended (older drivers drop connections).
- ChatGPT Desktop with Codex Micro support and an existing signed-in Codex
  session.
- A data-capable USB-C cable for the first flash. Normal use is wireless.

## Recommended installation: let Codex do it

This is the primary installation path. Open this repository's folder in the
Codex desktop app and paste the prompt below — Codex reads
[README.md](README.md) and [AGENTS.md](AGENTS.md), builds the firmware, flashes
it, and verifies the board.

### 1. Open the repository locally

Download this repository as a ZIP or clone it, then open its folder in the
Codex desktop app. Keep the repository on the Windows PC that will pair with
the watch.

### 2. Connect the watch

Connect the Waveshare 1.75C with a data-capable USB-C cable. Do not guess
which COM port belongs to it if other development boards are connected.

### 3. Paste this into Codex

```text
Install this project on my physical Waveshare ESP32-S3-Touch-AMOLED-1.75C.

Read AGENTS.md and README.md completely before acting. Work through the setup
autonomously, but follow these safety rules:

1. Start with read-only checks. Confirm Windows, the Waveshare 1.75C target,
   available build tools, and the exact newly connected COM port.
2. Explain any missing dependency before installing it. Never ask me for an
   OpenAI API key, login cookie, access token, or other credential.
3. Build the firmware (`pio run -e m5stack-stopwatch`, or
   `-c platformio.win.ini` on Windows) before attempting an upload.
4. Immediately before flashing, report the exact COM port you resolved and
   ask me to confirm that one destructive device action.
5. After flashing, verify the CODEX_MICRO_STOPWATCH_READY marker with
   `python scripts/serial_probe_win.py <the exact port> --seconds 30
   --expect CODEX_MICRO_STOPWATCH_READY`, then guide me through Windows
   Bluetooth pairing ("Codex Micro").
6. If controls do nothing after pairing, tell me to forget the device and
   pair again (Windows does not always re-subscribe HID notifications; the
   firmware forces the local CCCD, and a fresh pairing always fixes it).
7. Help me configure ChatGPT Desktop: BOOT = Push to talk, PWR = Toggle
   voice chat, center = Send, and let me choose the four swipe actions.
8. Quota sync is optional. If I approve, run
   `companion-win/setup_win_companion.ps1` in my own PowerShell and keep any
   generated paths, tokens, and logs out of Git.
9. Verify buttons, center Send, four swipes, Agent colors, completion chime,
   haptics, and a real quota/reset update separately. Report anything that
   was not physically observed as unverified.
```

The repository's [AGENTS.md](AGENTS.md) gives Codex durable installation and
privacy boundaries, so the prompt can stay readable. Claude Code and other
local coding agents can follow the same instructions.

### 4. Finish the visible Windows steps

Codex will handle the terminal work, but Windows may still require you to:

1. Approve installation of a missing build tool.
2. Confirm the exact COM port immediately before flashing.
3. Pair **Codex Micro** in **Settings > Bluetooth & devices**.
4. Run `companion-win\setup_win_companion.ps1` in your own PowerShell if you
   want quota sync (it needs your Codex login).
5. Configure the actions in **ChatGPT Desktop > Settings > Codex Micro**.

## Manual build and flash

The Codex-assisted flow is recommended. You can also build manually with
[PlatformIO Core](https://docs.platformio.org/en/latest/core/index.html):

```sh
pio run -e m5stack-stopwatch
pio run -e m5stack-stopwatch -t upload --upload-port COM5
```

Or use the one-click script (finds esptool automatically, no PlatformIO
needed):

```powershell
powershell -ExecutionPolicy Bypass -File flash_codex.ps1 -Port COM5
```

A successful boot prints:

```text
CODEX_MICRO_STOPWATCH_READY
```

Verify that marker against the same resolved port:

```sh
python scripts/serial_probe_win.py COM5 --seconds 30 --expect CODEX_MICRO_STOPWATCH_READY
```

## Controls

| Watch input | Reported control | Recommended Codex action |
| --- | --- | --- |
| Hold BOOT button | Mic key `ACT10` | Push to talk |
| Short press PWR button | Command Key 4 `ACT09` | Toggle voice chat |
| Tap the center quota dial | Send key `ACT12` | Send composer message |
| Swipe up / right / down / left | Analog stick directions | User configurable |
| Hold center dial for 6 seconds | Warned Travel Mode fallback | Power button or USB wakes it |

## Quota companion and privacy

The Codex Micro HID interface does not include account rate limits. The Windows
companion (`companion-win/`) reads your local Codex `auth.json` access token
(`CODEX_HOME`, or `~/.codex`) and calls the usage endpoint directly, then sends
only this small snapshot to the explicitly bound watch:

- remaining percentage;
- reset countdown;

It does not send an API key, access token, account identifier, prompt, task
text, or audio to the watch. Local paths, tokens, and logs must never be
committed. See [docs/PORTING_WAVESHARE.md](docs/PORTING_WAVESHARE.md) for the
port details.

## Differences vs the upstream C152

| Item | Upstream C152 | This port (Waveshare 1.75C) |
| --- | --- | --- |
| Side keys | Left/right physical buttons | BOOT (PTT) / PWR (voice chat) |
| Touch | 465×465 full resolution | CST9217 0~232 mirrored coords; firmware mirrors + scales |
| Power | M5PM1 | AXP2101 (I2C on GPIO14/15) |
| Audio | M5PM1 driven | ES8311 (manual I2S config) |
| BLE host | macOS (HOGP) | Windows (hidbthle, forced CCCD subscribe) |
| Quota sync | Swift companion | Windows Python (bleak) companion |

## Troubleshooting

### One tap pops the ChatGPT window several times / agent keeps flashing

The CST9217 can emit several press/release cycles for one physical tap. The
firmware applies a 600ms spot-lockout after each commit: trailing cycles at the
same spot are swallowed, while a press at a different spot (another agent, the
center Send dial) is allowed through immediately.

### Center dial does nothing

The center dial is the Send key. If you just tapped an agent, the 600ms
spot-lockout may swallow the center tap (anti-ghosting); tap again after a
moment.

### Bluetooth keeps dropping

- Upgrade the Intel Bluetooth driver to 24.x.
- Forget the device and pair again.
- Make sure no second "Codex Micro" device is connected.

### Controls work, then stop after the watch reconnects

Windows does not always re-subscribe HID notifications on reconnect. Delete
the pairing and pair again — the firmware forces the local CCCD, and a fresh
pairing always restores input.

## License / acknowledgements

MIT License. Ported from
[digitsisyph/codex-micro-stopwatch](https://github.com/digitsisyph/codex-micro-stopwatch)
and [imliubo/codex-micro-4-core2](https://github.com/imliubo/codex-micro-4-core2);
attribution preserved in [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md). Space
Mono font is under the SIL Open Font License 1.1.
