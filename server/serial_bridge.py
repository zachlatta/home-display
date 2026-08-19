#!/usr/bin/env python3
"""Scoped USB bridge between the E1002 and the local dashboard gateway."""

from __future__ import annotations

import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.request

import serial

ROOT = Path(__file__).resolve().parent.parent
PORT = os.environ.get("HOME_DISPLAY_SERIAL_PORT", "/dev/cu.usbserial-10")
URL = os.environ.get("HOME_DISPLAY_LOCAL_URL", "http://127.0.0.1:8787/api/dashboard")
TOKEN_FILE = Path(os.environ.get("HOME_DISPLAY_TOKEN_FILE", ROOT / ".secrets/dashboard-token"))
REQUEST = "SERIAL_DASHBOARD_REQUEST"
RESPONSE_PREFIX = b"SERIAL_DASHBOARD_RESPONSE "
MAX_PAYLOAD_BYTES = 65_536
# Must track PROTOCOL_VERSION in server/dashboard.mjs and src/dashboard_view.h.
PROTOCOL_VERSION = 5


def event(message: str) -> None:
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} {message}", flush=True)


def dashboard_payload() -> bytes:
    token = TOKEN_FILE.read_text(encoding="utf-8").strip()
    if len(token) < 32:
        raise RuntimeError("dashboard token is missing or too short")
    request = urllib.request.Request(
        URL,
        headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=15) as response:
        payload = response.read(MAX_PAYLOAD_BYTES + 1)
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise RuntimeError("dashboard response exceeds serial protocol limit")
    parsed = json.loads(payload)
    if parsed.get("protocolVersion") != PROTOCOL_VERSION:
        raise RuntimeError("unsupported dashboard protocol")
    return json.dumps(parsed, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def serve(connection: serial.Serial) -> None:
    while True:
        raw = connection.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if line == REQUEST:
            try:
                payload = dashboard_payload()
                connection.write(RESPONSE_PREFIX + payload + b"\n")
                connection.flush()
                event(f"dashboard_sent bytes={len(payload)}")
            except Exception as error:  # Keep bridge available for the next device retry.
                event(f"dashboard_send_failed type={type(error).__name__}")
        elif line.startswith(("HOME_DISPLAY_", "WIFI_", "DASHBOARD_", "SERIAL_", "SLEEP_")):
            event(f"device {line}")


def main() -> None:
    event(f"bridge_start port={PORT}")
    while True:
        try:
            with serial.Serial(PORT, 115200, timeout=1, write_timeout=5) as connection:
                event("device_connected")
                serve(connection)
        except (OSError, serial.SerialException, urllib.error.URLError) as error:
            event(f"device_disconnected type={type(error).__name__}")
            time.sleep(2)


if __name__ == "__main__":
    main()
