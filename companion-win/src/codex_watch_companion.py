# Windows quota companion for Codex Micro StopWatch (Waveshare port)
#
# Reads the current Codex rate-limit window from a local Codex App Server and
# writes a small quota snapshot to one explicitly bound watch over the
# project's private BLE service (7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01/02).
#
# This is a Windows port of companion/Sources/CodexWatchCompanion/main.swift
# using Python + bleak. It uses the user's existing local Codex/ChatGPT sign-in
# context; it never reads credentials directly or puts account tokens on the
# watch.
#
# Requirements:
#   pip install bleak
#   codex CLI available on PATH (npm i -g @openai/codex, or codex.exe)
#
# Usage:
#   python codex_watch_companion.py --demo --verbose
#   python codex_watch_companion.py --device-id <BLE_ADDR_OR_NAME> --watch --interval 60
#   python codex_watch_companion.py --json-only
"""
Windows Codex Watch Companion (bleak port).

Reads Codex quota from `codex app-server --listen stdio://` and writes the
snapshot to the watch over BLE GATT. Mirrors the macOS Swift companion's
protocol and safety rules:

- Only sends {"remaining_percent": n, "reset_in_seconds": n} (<= 512 bytes).
- Requires an explicitly bound device (BLE address or advertised name) for
  real writes; `--demo` writes synthetic data only.
- Refreshes at most once per minute in --watch mode.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass

QUOTA_SERVICE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01"
QUOTA_WRITE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02"
HID_SERVICE_UUID = "1812"
MAX_PAYLOAD = 512
DEFAULT_DEVICE_NAME = "Codex Micro"


class CompanionError(Exception):
    """Raised for any recoverable companion failure."""


@dataclass
class QuotaSnapshot:
    remaining_percent: int
    reset_in_seconds: int

    def to_json_bytes(self) -> bytes:
        payload = {
            "remaining_percent": max(0, min(100, self.remaining_percent)),
            "reset_in_seconds": max(0, int(self.reset_in_seconds)),
        }
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        if len(data) > MAX_PAYLOAD:
            raise CompanionError(f"payload {len(data)}B exceeds 512B limit")
        return data


# ---------------------------------------------------------------------------
# Codex App Server client (stdio JSON-RPC)
# ---------------------------------------------------------------------------

class AppServerClient:
    """Spawns `codex app-server --listen stdio://` and speaks JSON-RPC over stdio."""

    def __init__(self, codex_path: str | None = None):
        exe = codex_path or shutil.which("codex")
        # Windows: npm 在 PATH 里可能先解析到 codex.ps1；优先用 .cmd（同目录
        # 通常存在）。CreateProcess 不解析 .cmd/.ps1 扩展名，都需 shell 启动。
        if os.name == "nt" and exe:
            alt = os.path.splitext(exe)[0] + ".cmd"
            if os.path.isfile(alt):
                exe = alt
        if not exe:
            raise CompanionError(
                "codex 可执行文件未找到。请先安装：npm install -g @openai/codex"
            )
        # 同时显式设置 HOME 规避 app-server 启动失败（issue #20956）。
        self._use_shell = os.name == "nt"
        env = dict(os.environ)
        env.setdefault("HOME", os.path.expanduser("~"))
        self.proc = subprocess.Popen(
            [exe, "app-server", "--listen", "stdio://"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=False,
            env=env,
            shell=self._use_shell,
        )
        self._next_id = 1
        self._init_done = False

    def _request(self, method: str, params: dict, timeout: float = 20.0):
        req_id = self._next_id
        self._next_id += 1
        msg = json.dumps(
            {"method": method, "id": req_id, "params": params},
            separators=(",", ":"),
        ).encode("utf-8") + b"\n"
        assert self.proc.stdin is not None
        self.proc.stdin.write(msg)
        self.proc.stdin.flush()

        deadline = time.monotonic() + timeout
        assert self.proc.stdout is not None
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise CompanionError("Codex App Server 输出已关闭")
            try:
                obj = json.loads(line.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                # Windows 上 taskkill 等子进程可能把非 JSON 文本泄漏进
                # stdout（issue #21957 类），跳过而非崩溃。
                continue
            if obj.get("id") == req_id:
                if "error" in obj:
                    raise CompanionError(
                        f"Codex 请求失败: {obj['error'].get('message', obj['error'])}"
                    )
                return obj.get("result")
        raise CompanionError(f"等待 Codex App Server 响应超时 ({method})")

    def initialize(self):
        self._request(
            "initialize",
            {
                "clientInfo": {
                    "name": "codex_watch_companion",
                    "title": "Codex Watch Companion",
                    "version": "0.1.0",
                },
                "capabilities": {
                    "optOutNotificationMethods": [
                        "item/agentMessage/delta",
                        "item/reasoning/textDelta",
                    ]
                },
            },
            timeout=12,
        )
        # Newer codex app-server builds reject the JSON-RPC `initialized`
        # notification (unknown variant). `initialize` alone is sufficient.
        self._init_done = True

    def read_rate_limits(self) -> QuotaSnapshot:
        result = self._request("account/rateLimits/read", {})
        if not isinstance(result, dict):
            raise CompanionError("Codex 没有返回 rateLimits result")

        bucket = None
        rl_by_id = result.get("rateLimitsByLimitId")
        if isinstance(rl_by_id, dict):
            codex = rl_by_id.get("codex")
            if isinstance(codex, dict):
                bucket = codex
        if bucket is None:
            legacy = result.get("rateLimits")
            if isinstance(legacy, dict) and legacy.get("limitId") == "codex":
                bucket = legacy
        if bucket is None:
            # Free accounts may report a flat structure; be tolerant.
            primary = result.get("primary")
            if isinstance(primary, dict):
                bucket = {"primary": primary}

        if not isinstance(bucket, dict):
            raise CompanionError("没有找到 Codex 额度窗口")
        primary = bucket.get("primary")
        if not isinstance(primary, dict):
            raise CompanionError("没有找到 Codex primary 额度窗口")

        used = primary.get("usedPercent")
        resets_at = primary.get("resetsAt")
        if used is None or resets_at is None:
            raise CompanionError("primary 额度缺少 usedPercent/resetsAt")

        used_f = float(used)
        used_f = max(0.0, min(100.0, used_f))
        now = time.time()
        reset_in = max(0, int(float(resets_at) - now))
        return QuotaSnapshot(
            remaining_percent=int(round(100 - used_f)),
            reset_in_seconds=reset_in,
        )

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
# BLE writer (bleak)
# ---------------------------------------------------------------------------

async def find_and_write(
    payload: bytes,
    *,
    device_id: str | None,
    demo: bool,
    verbose: bool,
    timeout: float = 40.0,
) -> str:
    """Scan for the watch (by name or bound address) and write the payload.

    Returns a human-readable result message.
    """
    from bleak import BleakClient, BleakScanner

    want_name = device_id if device_id else DEFAULT_DEVICE_NAME

    def match(device, adv=None) -> bool:
        # Prefer the private quota service UUID in advertisement data (robust
        # against renamed/re-bonded devices), then fall back to name/address.
        if adv is not None:
            uuids = getattr(adv, "service_uuids", None) or []
            if any(str(u).lower() == QUOTA_SERVICE_UUID for u in uuids):
                return True
        name = (device.name or "").strip()
        if not name:
            return False
        if demo:
            return name == want_name
        if device_id:
            return device_id.lower() in (device.address or "").lower() or name == device_id
        return name == DEFAULT_DEVICE_NAME

    deadline = time.monotonic() + timeout
    device = None
    while time.monotonic() < deadline:
        # bleak 3.x: discover(return_adv=True) -> {address: (BLEDevice, AdvertisementData)}
        found = await BleakScanner.discover(timeout=5.0, return_adv=True)
        if isinstance(found, dict):
            items = found.values()
        else:
            items = found
        for entry in items:
            if isinstance(entry, tuple) and len(entry) == 2:
                dev, adv = entry
            else:
                dev, adv = entry, None
            if match(dev, adv):
                device = dev
                break
        if device:
            break
        if verbose:
            print("…仍在扫描 StopWatch…")

    if not device:
        raise CompanionError(
            f"未在 {int(timeout)} 秒内发现带专属服务的设备 (name={want_name})"
        )

    if verbose:
        print(f"发现 {device.name} [{device.address}]，连接并写入额度…")

    async with BleakClient(device.address, timeout=15.0) as client:
        svc = client.services.get_service(QUOTA_SERVICE_UUID)
        if not svc:
            # Some bonded HID connections cache old services; force discovery.
            await client.get_services()
            svc = client.services.get_service(QUOTA_SERVICE_UUID)
        if not svc:
            raise CompanionError("设备未暴露专属额度服务（确认固件为最新）")
        char = svc.get_characteristic(QUOTA_WRITE_UUID)
        if not char:
            raise CompanionError("设备未暴露额度写入特征")

        if verbose:
            print(f"写入 {len(payload)}B 额度快照…")
        await client.write_gatt_char(char, payload, response=False)
        return f"已写入额度快照到 {device.name} [{device.address}]"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

async def run_demo(verbose: bool) -> int:
    snapshot = QuotaSnapshot(remaining_percent=100, reset_in_seconds=3600)
    payload = snapshot.to_json_bytes()
    if verbose:
        print(f"demo 额度快照: {payload.decode()}")
    msg = await find_and_write(payload, device_id=None, demo=True, verbose=verbose)
    print(msg)
    return 0


async def run_real(
    device_id: str | None,
    watch: bool,
    interval: int,
    codex_path: str | None,
    verbose: bool,
) -> int:
    client = AppServerClient(codex_path)
    try:
        client.initialize()
        if verbose:
            print("Codex App Server 已初始化")
        while True:
            snapshot = client.read_rate_limits()
            payload = snapshot.to_json_bytes()
            if verbose:
                print(
                    f"额度: 剩余 {snapshot.remaining_percent}%, "
                    f"reset {snapshot.reset_in_seconds}s"
                )
            msg = await find_and_write(
                payload, device_id=device_id, demo=False, verbose=verbose
            )
            print(msg)
            if not watch:
                break
            time.sleep(max(10, interval))
    finally:
        client.close()
    return 0


async def run_json_only(codex_path: str | None, verbose: bool) -> int:
    client = AppServerClient(codex_path)
    try:
        client.initialize()
        snapshot = client.read_rate_limits()
        print(
            json.dumps(
                {
                    "remaining_percent": snapshot.remaining_percent,
                    "reset_in_seconds": snapshot.reset_in_seconds,
                }
            )
        )
        return 0
    finally:
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Codex Watch Companion (Windows)")
    parser.add_argument("--demo", action="store_true", help="写入合成数据（发现模式）")
    parser.add_argument("--device-id", help="绑定的 BLE 地址或设备名")
    parser.add_argument("--watch", action="store_true", help="持续刷新")
    parser.add_argument("--interval", type=int, default=60, help="刷新间隔（秒）")
    parser.add_argument("--codex-path", help="codex 可执行文件路径")
    parser.add_argument("--json-only", action="store_true", help="只打印额度 JSON")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    try:
        if args.json_only:
            return asyncio.run(run_json_only(args.codex_path, args.verbose))
        if args.demo:
            return asyncio.run(run_demo(args.verbose))
        return asyncio.run(
            run_real(
                args.device_id, args.watch, args.interval, args.codex_path, args.verbose
            )
        )
    except (CompanionError, OSError) as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("已停止")
        return 130


if __name__ == "__main__":
    sys.exit(main())
