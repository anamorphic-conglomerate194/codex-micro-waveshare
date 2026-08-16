# Codex Watch All-in-one (Windows): quota sync + button/joystick bridge in one
# BLE connection.
#
# Combines codex_watch_companion.py (read Codex quota, write to watch GATT) and
# codex_watch_bridge.py (subscribe to watch HID input, drive Codex turns) into a
# single process. One BLE connection, one Codex App Server: the watch shows real
# quota AND its buttons/joystick drive the agent.
#
# Usage:
#   py codex_watch_all.py --verbose
#   py codex_watch_all.py --interval 60 --verbose
#
# Requirements: pip install bleak ; codex CLI on PATH, logged in.
from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import time

# CODEX_STOPWATCH_THIRD_PARTY: prefer CODEX_HOME from the environment when it
# is set; otherwise fall back to the standard ~/.codex location. A local
# workspace override can be provided at runtime via CODEX_HOME (e.g. when the
# user home directory is not writable in a sandboxed environment).
if "CODEX_HOME" not in os.environ:
    _default_home = os.path.join(os.path.expanduser("~"), ".codex")
    if os.path.isdir(_default_home):
        os.environ["CODEX_HOME"] = _default_home

QUOTA_SERVICE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01"
QUOTA_WRITE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02"
# Vendor control service exposed by the Waveshare firmware (Windows bleak does
# not enumerate the standard 0x1812 HID service).
CONTROL_SERVICE_UUID = "5f9d4e66-2ac2-4a71-bfbe-4ef61a0e5c11"
HID_INPUT_CHAR = "00002a4d-0000-1000-8000-00805f9b34fb"
MAX_PAYLOAD = 512


class AppError(Exception):
    pass


# ---------------------------------------------------------------------------
# Windows keyboard simulation (ctypes SendInput, no extra dependency)
# ---------------------------------------------------------------------------

import ctypes
from ctypes import wintypes

# Virtual-key codes
VK_CONTROL = 0x11
VK_SHIFT = 0x10
VK_MENU = 0x12
VK_RETURN = 0x0D
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_EXTENDEDKEY = 0x0001

INPUT_KEYBOARD = 1


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class INPUT(ctypes.Structure):
    class _INPUT(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("padding", ctypes.c_byte * 24)]

    _fields_ = [("type", wintypes.DWORD), ("_input", _INPUT)]


def _key_press(vk: int, keyup: bool):
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp._input.ki.wVk = vk
    inp._input.ki.dwFlags = KEYEVENTF_KEYUP if keyup else 0
    ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def find_window_by_title(fragment: str):
    """Find a top-level window whose title contains `fragment`."""
    result = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _lparam):
        length = ctypes.windll.user32.GetWindowTextLengthW(hwnd)
        if length > 0:
            buf = ctypes.create_unicode_buffer(length + 1)
            ctypes.windll.user32.GetWindowTextW(hwnd, buf, length + 1)
            if fragment.lower() in buf.value.lower():
                result.append(hwnd)
        return True

    ctypes.windll.user32.EnumWindows(cb, 0)
    return result[0] if result else None


def activate_window(hwnd):
    try:
        ctypes.windll.user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        ctypes.windll.user32.SetForegroundWindow(hwnd)
    except Exception:
        pass


def trigger_codex_voice():
    """Bring the Codex window forward and press Ctrl+Shift+D (Codex default
    voice chat hotkey; V was a mistaken earlier guess)."""
    # Common Codex/ChatGPT window titles.
    for title in ("Codex", "ChatGPT"):
        hwnd = find_window_by_title(title)
        if hwnd:
            activate_window(hwnd)
            import time as _t
            _t.sleep(0.2)
            break
    send_hotkey(True, True, False, ord("D"))


def send_hotkey(ctrl: bool, shift: bool, alt: bool, vk: int):
    """Press a hotkey combo (e.g. Ctrl+Shift+V) with human-like timing, then
    release. Slower sequencing helps apps distinguish the chord from a fast
    mechanical tap (which some apps ignore as a 'bounce')."""
    import time as _t
    if ctrl:
        _key_press(VK_CONTROL, False)
        _t.sleep(0.05)
    if shift:
        _key_press(VK_SHIFT, False)
        _t.sleep(0.05)
    if alt:
        _key_press(VK_MENU, False)
        _t.sleep(0.05)
    _key_press(vk, False)
    _t.sleep(0.08)
    _key_press(vk, True)
    _t.sleep(0.05)
    if alt:
        _key_press(VK_MENU, True)
        _t.sleep(0.05)
    if shift:
        _key_press(VK_SHIFT, True)
        _t.sleep(0.05)
    if ctrl:
        _key_press(VK_CONTROL, True)
        _t.sleep(0.05)


def send_enter():
    send_hotkey(False, False, False, VK_RETURN)



# ---------------------------------------------------------------------------
# Codex App Server client (stdio JSON-RPC): quota + turns
# ---------------------------------------------------------------------------

class CodexClient:
    def __init__(self, codex_path: str | None = None, verbose: bool = False):
        self.verbose = verbose
        self.proc = None
        self._next_id = 1
        self._thread_id: str | None = None
        # CODEX_STOPWATCH_THIRD_PARTY: the app-server process is optional now.
        # Quota is read directly from the ChatGPT wham/usage endpoint with the
        # saved access token (the app-server quota path has been flaky on this
        # machine), and the bridge no longer drives Codex turns.
        exe = codex_path or shutil.which("codex")
        if os.name == "nt" and exe:
            alt = os.path.splitext(exe)[0] + ".cmd"
            if os.path.isfile(alt):
                exe = alt
        if exe:
            try:
                env = dict(os.environ)
                env.setdefault("HOME", os.path.expanduser("~"))
                self.proc = subprocess.Popen(
                    [exe, "app-server", "--listen", "stdio://"],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=False, env=env, shell=os.name == "nt",
                )
            except Exception as exc:
                if verbose:
                    print(f"app-server start skipped: {exc}", file=sys.stderr)
                self.proc = None
        elif verbose:
            print("codex CLI not found; direct quota mode only", file=sys.stderr)

    def _load_auth(self) -> dict:
        import pathlib
        home = os.environ.get("CODEX_HOME") or os.path.join(os.path.expanduser("~"), ".codex")
        p = pathlib.Path(home) / "auth.json"
        if not p.exists():
            raise AppError(f"no auth.json at {p}; run: codex login")
        with open(p, encoding="utf-8") as f:
            return json.load(f)

    def _request(self, method: str, params: dict, timeout: float = 20.0):
        if self.proc is None:
            raise AppError("Codex App Server not running")
        rid = self._next_id
        self._next_id += 1
        msg = (json.dumps({"method": method, "id": rid, "params": params},
                          separators=(",", ":")) + "\n").encode("utf-8")
        assert self.proc.stdin is not None
        self.proc.stdin.write(msg)
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        assert self.proc.stdout is not None
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise AppError("Codex App Server output closed")
            try:
                obj = json.loads(line.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue  # skip taskkill-like pollution
            if obj.get("id") == rid:
                if "error" in obj:
                    raise AppError(f"Codex {method} failed: {obj['error'].get('message', obj['error'])}")
                return obj.get("result")
        raise AppError(f"Codex App Server timeout ({method})")

    def initialize(self):
        if self.proc is not None:
            try:
                self._request("initialize", {
                    "clientInfo": {"name": "codex_watch_all", "title": "Codex Watch", "version": "0.2.0"},
                    "capabilities": {"optOutNotificationMethods": ["item/agentMessage/delta", "item/reasoning/textDelta"]},
                }, timeout=12)
            except AppError as exc:
                if self.verbose:
                    print(f"app-server initialize skipped: {exc}", file=sys.stderr)

    # --- quota ---

    def read_quota(self) -> dict:
        # Direct call to the ChatGPT wham/usage endpoint with the saved access
        # token. More reliable than the codex app-server quota path on this
        # machine (its network/TLS requests to chatgpt.com were failing).
        import urllib.request
        auth = self._load_auth()
        tokens = auth.get("tokens") or {}
        token = tokens.get("access_token")
        if not token:
            raise AppError("auth.json has no access_token; run: codex login")
        req = urllib.request.Request(
            "https://chatgpt.com/backend-api/wham/usage",
            headers={"Authorization": "Bearer " + token,
                     "User-Agent": "codex-watch-companion/0.2"},
        )
        with urllib.request.urlopen(req, timeout=20) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        pw = (data.get("rate_limit") or {}).get("primary_window")
        if not isinstance(pw, dict):
            raise AppError("no primary_window in wham/usage response")
        used = pw.get("used_percent")
        reset_in = pw.get("reset_after_seconds")
        if used is None or reset_in is None:
            raise AppError("primary_window missing used_percent/reset_after_seconds")
        used_f = max(0.0, min(100.0, float(used)))
        return {"remaining_percent": int(round(100 - used_f)),
                "reset_in_seconds": max(0, int(float(reset_in)))}

    # --- turns ---

    def _find_thread_id(self, obj) -> str | None:
        if isinstance(obj, dict):
            for key in ("id", "threadId", "thread"):
                val = obj.get(key)
                if isinstance(val, str) and len(val) > 8:
                    return val
                if isinstance(val, dict):
                    found = self._find_thread_id(val)
                    if found:
                        return found
            for val in obj.values():
                if isinstance(val, (dict, list)):
                    found = self._find_thread_id(val)
                    if found:
                        return found
        elif isinstance(obj, list):
            for item in obj:
                found = self._find_thread_id(item)
                if found:
                    return found
        return None

    def ensure_thread(self) -> str:
        if self._thread_id:
            return self._thread_id
        result = self._request("thread/start", {})
        self._thread_id = self._find_thread_id(result)
        if not self._thread_id:
            raise AppError("thread/start did not return a thread id")
        if self.verbose:
            print(f"thread ready: {self._thread_id}")
        return self._thread_id

    def send_message(self, text: str) -> None:
        thread = self.ensure_thread()
        if self.verbose:
            print(f"-> turn/start: {text!r}")
        self._request("turn/start", {"threadId": thread,
                                     "input": [{"type": "text", "text": text}]})

    def close(self):
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=3)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass


# ---------------------------------------------------------------------------
# BLE event handling
# ---------------------------------------------------------------------------

def parse_reports(data: bytes) -> list[str]:
    offset = 2 if len(data) >= 2 and data[0] == 2 else 0
    text = data[offset:].decode("utf-8", "replace")
    return [line for line in text.split("\n") if line.strip()]


class WatchHandler:
    def __init__(self, codex: CodexClient, verbose: bool):
        self.codex = codex
        self.verbose = verbose

    def handle_json(self, obj: dict):
        method = obj.get("method")
        params = obj.get("params") or {}
        if method == "v.oai.hid":
            self.handle_key(params.get("k", ""), params.get("act", 0),
                            params.get("ag", -1))
        elif method == "v.oai.rad":
            self.handle_joystick(float(params.get("a", 0.0)),
                                 float(params.get("d", 0.0)))
        elif self.verbose:
            print(f"(ignored method {method})")

    def handle_key(self, key: str, action: int, agent: int):
        if self.verbose:
            print(f"HID key={key} act={action} agent={agent}")
        try:
            if key == "ACT10":
                # Mic / Push-to-talk: trigger the Codex voice input hotkey.
                # Press = start (Ctrl+Shift+D), Release = the app stops when
                # the hotkey toggles, so only act on press.
                if action == 1:
                    trigger_codex_voice()
                    if self.verbose:
                        print("-> Ctrl+Shift+D (Codex voice input)")
            elif key == "ACT09":
                # Voice chat toggle (Command Key 4) -> same voice hotkey.
                if action == 1:
                    trigger_codex_voice()
                    if self.verbose:
                        print("-> Ctrl+Shift+D (voice chat toggle)")
            elif key == "ACT12":
                # Send: press Enter in the Codex composer.
                if action == 1:
                    send_enter()
                    if self.verbose:
                        print("-> Enter (send)")
            elif key.startswith("AG"):
                # Agent keys: fall back to a codex turn message for now.
                self.codex.send_message(f"(agent {agent + 1} selected)")
        except AppError as exc:
            print(f"turn error: {exc}", file=sys.stderr)

    def handle_joystick(self, angle: float, distance: float):
        if self.verbose:
            print(f"JOYSTICK angle={angle:.2f} dist={distance:.2f}")
        if distance <= 0:
            return
        if angle <= 45 or angle > 315:
            direction = "up"
        elif angle <= 135:
            direction = "right"
        elif angle <= 225:
            direction = "down"
        else:
            direction = "left"
        try:
            self.codex.send_message(f"(joystick:{direction})")
        except AppError as exc:
            print(f"turn error: {exc}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

async def run(codex_path: str | None, interval: int, verbose: bool):
    from bleak import BleakClient, BleakScanner

    codex = CodexClient(codex_path, verbose=verbose)
    codex.initialize()
    if verbose:
        print("Codex App Server initialized")

    print("Scanning for Codex Micro...")
    deadline = time.monotonic() + 40
    device = None
    while time.monotonic() < deadline and not device:
        found = await BleakScanner.discover(timeout=5.0)
        for d in found:
            if (d.name or "").strip() == "Codex Micro":
                device = d
                break
    if not device:
        raise AppError("Watch not found (is it powered on and advertising?)")
    if verbose:
        print(f"Found {device.name} [{device.address}]")

    handler = WatchHandler(codex, verbose)

    def on_notify(_sender, data: bytearray):
        for line in parse_reports(bytes(data)):
            try:
                obj = json.loads(line)
                handler.handle_json(obj)
            except json.JSONDecodeError:
                pass  # partial/fill bytes

    # One BLE connection for both quota writes and button notifications.
    # The watch uses a random BLE address and Windows scans are lossy, so we
    # use a continuous callback scanner (not snapshots) and reconnect forever.
    watch_device = [None]  # boxed so the callback can set it

    def detection_callback(advertised_device, adv):
        if watch_device[0] is None and \
                (advertised_device.name or "").strip() == "Codex Micro":
            watch_device[0] = advertised_device

    while True:
        watch_device[0] = None
        scanner = BleakScanner(detection_callback=detection_callback)
        await scanner.start()
        try:
            scan_deadline = time.monotonic() + 15
            while time.monotonic() < scan_deadline and watch_device[0] is None:
                await asyncio.sleep(0.3)
        finally:
            await scanner.stop()
        device = watch_device[0]
        if not device:
            print("watch not advertising; retrying in 5s...", file=sys.stderr)
            await asyncio.sleep(5)
            continue

        if verbose:
            print(f"connecting {device.name} [{device.address}]")
        try:
            # winrt: bypass the OS service cache. Without this, a freshly
            # flashed control service is not discovered (stale cache).
            client_kwargs = {}
            if os.name == "nt":
                client_kwargs["winrt"] = {"use_cached_services": False}
            async with BleakClient(device, timeout=20.0, **client_kwargs) as client:
                if verbose:
                    print("connected; discovered services:")
                    for s in client.services:
                        print("   SVC", s.uuid)
                ctrl = client.services.get_service(CONTROL_SERVICE_UUID)
                if not ctrl:
                    # CODEX_STOPWATCH_THIRD_PARTY: current firmware exposes the
                    # vendor JSON on the REAL 0x1812 HID service, which Windows
                    # hides from user-space GATT (the HID driver owns it). The
                    # ChatGPT app reads reports natively, so the bridge's button
                    # forwarding is not needed; keep quota sync only.
                    print("control service not exposed; HID mode (buttons handled by ChatGPT app)")
                    ctrl = None
                char = None
                if ctrl is not None:
                    # The control service has TWO 0x2A4D characteristics (input with
                    # notify and output with write). Pick the one with notify.
                    for c in ctrl.characteristics:
                        if str(c.uuid).lower() == HID_INPUT_CHAR and "notify" in c.properties:
                            char = c
                            break
                    if not char:
                        raise AppError("input characteristic 0x2A4D (notify) not found")
                    await client.start_notify(char, on_notify)
                    print("Subscribed to watch input. Buttons/joystick drive Codex.")

                quota_svc = client.services.get_service(QUOTA_SERVICE_UUID)
                quota_char = quota_svc.get_characteristic(QUOTA_WRITE_UUID) if quota_svc else None
                # CODEX_STOPWATCH_THIRD_PARTY: write ONE quota snapshot per
                # connection, then disconnect. Holding the bleak connection
                # continuously steals the single Windows LE link from the HID
                # driver, which kills push-to-talk voice while the companion
                # runs. A short connect/write/disconnect every <interval>s
                # leaves the HID session available the rest of the time.
                try:
                    if quota_char:
                        try:
                            snapshot = codex.read_quota()
                            payload = json.dumps(snapshot, separators=(",", ":")).encode("utf-8")
                            if len(payload) <= MAX_PAYLOAD:
                                await client.write_gatt_char(quota_char, payload, response=False)
                                if verbose:
                                    print(f"quota: {snapshot}")
                            else:
                                print(f"quota payload too large ({len(payload)}B)", file=sys.stderr)
                        except AppError as exc:
                            print(f"quota error: {exc}", file=sys.stderr)
                except asyncio.CancelledError:
                    raise
                finally:
                    if char is not None:
                        await client.stop_notify(char)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            print(f"connection lost ({exc}); reconnecting in 5s...", file=sys.stderr)
            await asyncio.sleep(5)
            continue
        # Normal exit of the async-with means the client disconnected. Wait the
        # full interval (disconnected) so the HID driver keeps the link between
        # quota snapshots.
        print(f"disconnected; next quota in {interval}s...", file=sys.stderr)
        await asyncio.sleep(interval)

    codex.close()


def main():
    parser = argparse.ArgumentParser(description="Codex Watch All-in-one (Windows)")
    parser.add_argument("--device-id", default="Codex Micro")
    parser.add_argument("--codex-path", help="codex executable path")
    parser.add_argument("--interval", type=int, default=60, help="quota refresh seconds")
    # Compatibility: the old companion accepted --watch; keep accepting it so
    # the scheduled task command line does not need to change.
    parser.add_argument("--watch", action="store_true", help="(compat) continuous refresh")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    try:
        asyncio.run(run(args.codex_path, args.interval, args.verbose))
    except (AppError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nStopped")
        sys.exit(130)


if __name__ == "__main__":
    main()
