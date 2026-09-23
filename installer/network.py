"""Post-install identity and runtime checks over Ethernet."""

from __future__ import annotations

import json
import socket
import time
import urllib.request


def _get_json(ip: str, path: str) -> dict:
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(f"http://{ip}{path}", timeout=3) as response:
        if response.status != 200:
            raise ValueError(f"HTTP {response.status} for {path}")
        data = response.read(32769)
    if len(data) > 32768:
        raise ValueError("Board response exceeded its size limit")
    return json.loads(data)


def _discover(uid: str, seconds: int = 35) -> str | None:
    deadline = time.monotonic() + seconds
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("0.0.0.0", 0))
        sock.settimeout(1)
        while time.monotonic() < deadline:
            for destination in ("255.255.255.255", "192.168.2.255"):
                try:
                    sock.sendto(b"G100_DISCOVER_V1", (destination, 37020))
                except OSError:
                    pass
            until = min(deadline, time.monotonic() + 2)
            while time.monotonic() < until:
                try:
                    payload, address = sock.recvfrom(4096)
                    response = json.loads(payload)
                    if response.get("uid", "").upper() == uid.upper():
                        return address[0]
                except (OSError, ValueError, json.JSONDecodeError):
                    break
    return None


def test_runtime(uid: str, version: str, image_bytes: int) -> dict:
    ip = _discover(uid)
    if not ip:
        return {"status": "unreachable", "reason":
                "No matching UID answered UDP discovery. Check Ethernet, DHCP and host routing."}
    try:
        # Recovery intentionally opens a rescue window before a confirmed app boots.
        deadline = time.monotonic() + 75
        while True:
            try:
                identity = _get_json(ip, "/api/v1/identity")
                if identity.get("device_id", "").upper() != uid.upper():
                    raise ValueError("HTTP identity does not match the selected UID")
                if identity.get("firmware_version") == version:
                    break
            except OSError:
                pass
            if time.monotonic() >= deadline:
                raise ValueError("Expected application did not start after the recovery window")
            time.sleep(1)
            replacement = _discover(uid, seconds=2)
            if replacement:
                ip = replacement
        health = _get_json(ip, "/api/v1/health")
        memory = _get_json(ip, "/api/v1/system/memory")
        checks = {
            "identity_uid": identity.get("device_id", "").upper() == uid.upper(),
            "firmware_version": identity.get("firmware_version") == version,
            "health": health.get("status") == "ok" and health.get("management_ready") is True,
            "architecture": memory.get("architecture") == "nor-sdram-shadow-v1",
            "image_bytes": memory.get("image_bytes") == image_bytes,
            "boot_confirmed": memory.get("boot_confirmed") is True,
            "sdram_self_test": memory.get("sdram_test_errors") == 0 and
                               memory.get("sdram_test_passes", 0) > 0,
            "nor_capacity": memory.get("nor_bytes") == 16777216,
            "sdram_capacity": memory.get("sdram_bytes") == 16777216,
            "sd_disabled": memory.get("sd_in_use") is False,
        }
        return {"status": "passed" if all(checks.values()) else "failed",
                "ip": ip, "checks": checks, "identity": identity,
                "health": health, "memory": memory}
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        return {"status": "failed", "ip": ip, "reason": str(exc)}
