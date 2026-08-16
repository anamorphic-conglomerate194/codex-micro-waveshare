# Codex Watch Bridge (Windows): listen to the watch's BLE HID output and drive
# a local Codex App Server turn.
#
# The watch (Waveshare port, "Codex Micro" compatible) emits its button/joystick
# actions as JSON over the HID input characteristic (0x2A4D, report id 2,
# chunked). This bridge subscribes to that characteristic, parses the
# `v.oai.hid` / `v.oai.rad` messages, and calls Codex App Server APIs
# (thread/start, turn/start, turn/steer, turn/interrupt) so the physical
# buttons/joystick actually drive the agent.
#
# Usage:
#   py codex_watch_bridge.py --verbose
#   py codex_watch_bridge.py --device-id "Codex Micro" --verbose
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

QUOTA_SERVICE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01"
# CODEX_STOPWATCH_THIRD_PARTY: the watch exposes its vendor control JSON on
# this private service (Windows bleak does not enumerate the standard 0x1812
# HID service). The input-report characteristic is 0x2A4D with notify.
CONTROL_SERVICE_UUID = "5f9d4e66-2ac2-4a71-bfbe-4ef61a0e5c11"
HID_INPUT_CHAR = "00002a4d-0000-1000-8000-00805f9b34fb"

# Report framing used by the firmware: report[0]=2 (report id), report[1]=len,
# report[2..]=JSON chunk. HOGP may strip the report id (report[0]==len).
REPORT_ID = 2


class BridgeError(Exception):
    pass


class CodexClient:
    """Thin JSON-RPC client over `codex app-server --listen stdio://`."""

    def __init__(self, codex_path: str | None = None, verbose: bool = False):
        exe = codex_path or shutil.which("codex")
        if os.name == "nt" and exe:
            alt = os.path.splitext(exe)[0] + ".cmd"
            if os.path.isfile(alt):
                exe = alt
        if not exe:
            raise BridgeError("codex CLI not found. Run: npm install -g @openai/codex")
        self.verbose = verbose
        env = dict(os.environ)
        env.setdefault("HOME", os.path.expanduser("~"))
        self.proc = subprocess.Popen(
            [exe, "app-server", "--listen", "stdio://"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=False,
            env=env,
            shell=os.name == "nt",
        )
        self._next_id = 1
        self._thread_id: str | None = None

    def _request(self, method: str, params: dict, timeout: float = 20.0):
        rid = self._next_id
        self._next_id += 1
        msg = (
            json.dumps({"method": method, "id": rid, "params": params},
                       separators=(",", ":")) + "\n"
        ).encode("utf-8")
        assert self.proc.stdin is not None
        self.proc.stdin.write(msg)
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        assert self.proc.stdout is not None
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise BridgeError("Codex App Server output closed")
            try:
                obj = json.loads(line.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue  # skip taskkill-like pollution
            if obj.get("id") == rid:
                if "error" in obj:
                    raise BridgeError(
                        f"Codex {method} failed: {obj['error'].get('message', obj['error'])}"
                    )
                return obj.get("result")
        raise BridgeError(f"Codex App Server timeout ({method})")

    def initialize(self):
        self._request(
            "initialize",
            {
                "clientInfo": {"name": "codex_watch_bridge", "title": "Codex Watch Bridge", "version": "0.1.0"},
                "capabilities": {"optOutNotificationMethods": ["item/agentMessage/delta", "item/reasoning/textDelta"]},
            },
            timeout=12,
        )

    def _find_thread_id(self, obj) -> str | None:
        """Recursively look for a thread id in a thread/start result."""
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
            raise BridgeError("thread/start did not return a thread id")
        if self.verbose:
            print(f"thread ready: {self._thread_id}")
        return self._thread_id

    def send_message(self, text: str, verbose: bool = False) -> None:
        thread = self.ensure_thread()
        if verbose:
            print(f"-> turn/start: {text!r}")
        # Newer codex app-server expects `input` as a sequence of items.
        self._request("turn/start", {"threadId": thread, "input": [{"type": "text", "text": text}]})
        # Streaming events are notifications (no id); we fire-and-forget the turn.

    def steer(self, text: str, verbose: bool = False) -> None:
        thread = self.ensure_thread()
        if verbose:
            print(f"-> turn/steer: {text!r}")
        self._request("turn/steer", {"threadId": thread, "input": [{"type": "text", "text": text}]})

    def interrupt(self, verbose: bool = False) -> None:
        if not self._thread_id:
            return
        if verbose:
            print("-> turn/interrupt")
        self._request("turn/interrupt", {"threadId": self._thread_id})

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
# Report parsing
# ---------------------------------------------------------------------------

def parse_reports(data: bytes) -> list[str]:
    """Split a notification chunk into complete JSON strings.

    Firmware framing: [report_id?=2, len, payload...]. A single notify carries
    up to kReportBodySize bytes; a JSON message may span multiple notifies, so
    we accumulate and split on newline (firmware appends '\\n').
    """
    # HOGP strips report id: if data[0] == REPORT_ID and data[1] == len-1 -> id present.
    offset = 0
    if len(data) >= 2 and data[0] == REPORT_ID:
        offset = 2
    # Remaining bytes are payload (chunk length already consumed by transport).
    payload = data[offset:]
    text = payload.decode("utf-8", "replace")
    return [line for line in text.split("\n") if line.strip()]


class WatchBridge:
    def __init__(self, codex: CodexClient, verbose: bool):
        self.codex = codex
        self.verbose = verbose
        self.buffer = ""

    def handle_json(self, obj: dict):
        method = obj.get("method")
        params = obj.get("params") or {}
        if method == "v.oai.hid":
            key = params.get("k", "")
            action = params.get("act", 0)
            agent = params.get("ag", -1)
            self.handle_key(key, action, agent)
        elif method == "v.oai.rad":
            angle = params.get("a", 0.0)
            dist = params.get("d", 0.0)
            self.handle_joystick(float(angle), float(dist))
        else:
            if self.verbose:
                print(f"(ignored method {method})")

    def handle_key(self, key: str, action: int, agent: int):
        name = {
            "ACT10": "left/PTT",
            "ACT09": "right/voice",
            "ACT12": "send",
        }.get(key, key)
        if self.verbose:
            print(f"HID key={key} ({name}) act={action} agent={agent}")
        if action == 1:  # press
            if key == "ACT10":
                # Push to talk: send a text prompt on press (a future version
                # can capture microphone audio and transcribe it here).
                self.codex.send_message("(Push-to-talk pressed)", verbose=self.verbose)
            elif key == "ACT09":
                self.codex.send_message("(Toggle voice chat)", verbose=self.verbose)
            elif key == "ACT12":
                self.codex.send_message("(Send)", verbose=self.verbose)
        # action == 0 (release) is intentionally ignored: turn/steer needs
        # expectedTurnId tracking we don't want to fake for button presses.

    def handle_joystick(self, angle: float, distance: float):
        if self.verbose:
            print(f"JOYSTICK angle={angle:.2f} dist={distance:.2f}")
        if distance <= 0:
            return
        # Map angle to a direction for the agent (up/right/down/left).
        if angle <= 45 or angle > 315:
            direction = "up"
        elif angle <= 135:
            direction = "right"
        elif angle <= 225:
            direction = "down"
        else:
            direction = "left"
        self.codex.send_message(f"(joystick:{direction})", verbose=self.verbose)


async def main():
    parser = argparse.ArgumentParser(description="Codex Watch Bridge (Windows)")
    parser.add_argument("--device-id", default="Codex Micro", help="watch name or BLE address")
    parser.add_argument("--codex-path", help="codex executable path")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    from bleak import BleakClient, BleakScanner

    codex = CodexClient(args.codex_path, verbose=args.verbose)
    codex.initialize()
    if args.verbose:
        print("Codex App Server initialized")

    # Scan for the watch
    print(f"Scanning for {args.device_id}...")
    deadline = time.monotonic() + 40
    device = None
    while time.monotonic() < deadline and not device:
        found = await BleakScanner.discover(timeout=5.0)
        for d in found:
            name = (d.name or "").strip()
            if args.device_id.lower() in (d.address or "").lower() or name == args.device_id:
                device = d
                break
    if not device:
        raise BridgeError(f"Watch not found (looked for {args.device_id})")
    if args.verbose:
        print(f"Found {device.name} [{device.address}]")

    bridge = WatchBridge(codex, args.verbose)

    def on_notify(_sender, data: bytearray):
        for line in parse_reports(bytes(data)):
            try:
                obj = json.loads(line)
                bridge.handle_json(obj)
            except json.JSONDecodeError:
                if args.verbose:
                    print(f"(partial json: {line!r})")

    async with BleakClient(device.address, timeout=15.0) as client:
        # bleak 3.x: services are discovered automatically on connect; the
        # services dict is available as client.services.
        svc = client.services.get_service(CONTROL_SERVICE_UUID)
        if not svc:
            raise BridgeError("control service not found (is the watch running the latest firmware?)")
        char = svc.get_characteristic(HID_INPUT_CHAR)
        if not char:
            raise BridgeError("input characteristic (0x2A4D) not found in control service")
        await client.start_notify(char, on_notify)
        print("Subscribed to watch HID output. Buttons/joystick now drive Codex.")
        print("Press Ctrl+C to stop.")
        try:
            while True:
                await asyncio.sleep(1.0)
        except asyncio.CancelledError:
            pass
        finally:
            await client.stop_notify(char)

    codex.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (BridgeError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nStopped")
        sys.exit(130)
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
