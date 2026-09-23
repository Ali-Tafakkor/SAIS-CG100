"""Authenticated binary firmware client. This module cannot access an SWD probe."""
from __future__ import annotations
import hashlib
import hmac
import ipaddress
import json
import socket
import struct
import time
import urllib.error
import urllib.request
from image_format import pack, HEADER

PREFIX = "/api/v1/firmware/"

class UpdateError(RuntimeError):
    pass

class AuthenticationError(UpdateError):
    pass

class Client:
    def __init__(self, ip: str, uid: str, key: bytes, *, port: int = 80, timeout: float = 20):
        self.ip = str(ipaddress.IPv4Address(ip))
        if len(key) != 32:
            raise ValueError("A provisioned 256-bit update key is required")
        self.uid, self.key = uid.upper(), key
        self.base, self.timeout = f"http://{self.ip}:{port}", timeout
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.nonce, self.sequence = b"", 0

    def _http(self, method, path, data=None):
        request = urllib.request.Request(self.base + path, data=data, method=method,
            headers={"Content-Type": "application/octet-stream", "Connection": "close"})
        try:
            response = self.opener.open(request, timeout=self.timeout)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read(8193)
            if len(raw) > 8192:
                raise UpdateError("Board response exceeds the protocol limit")
            return response.code, response.headers, raw

    def challenge(self):
        code, _, raw = self._http("GET", PREFIX + "status")
        status = json.loads(raw)
        if code != 200 or status.get("protocol") != 2:
            raise UpdateError("Install the network bootstrap once via SWD; this board lacks protocol v2")
        if status.get("uid", "").upper() != self.uid:
            raise AuthenticationError("Discovered IP belongs to a different board UID")
        if status.get("auth_ready") is not True:
            raise AuthenticationError("Board key or hardware random generator is unavailable")
        try:
            nonce, sequence = bytes.fromhex(status["nonce"]), status["sequence"]
            if len(nonce) != 32 or type(sequence) is not int or not 0 < sequence < 0xFFFFFFFF:
                raise ValueError()
        except (KeyError, ValueError, TypeError) as exc:
            raise AuthenticationError("Invalid authentication challenge") from exc
        self.nonce, self.sequence = nonce, sequence

    def call(self, action: str, payload: bytes = b"", *, retry: bool = True):
        path = PREFIX + action
        for attempt in range(3 if retry else 1):
            try:
                if not self.nonce:
                    self.challenge()
                seq = struct.pack("<I", self.sequence)
                mac = hmac.digest(self.key, self.nonce + seq + path.encode("ascii") + b"\0" + payload, "sha256")
                code, headers, raw = self._http("POST", path, seq + mac + payload)
                expected = hmac.digest(self.key, self.nonce + seq + b"response\0" + raw, "sha256").hex()
                if not hmac.compare_digest(headers.get("X-G100-MAC", ""), expected):
                    raise AuthenticationError("Board response could not be authenticated; no success was accepted")
                self.sequence += 1
                result = json.loads(raw)
                if code >= 400 or result.get("ok") is not True:
                    raise UpdateError(f"{action.split('?')[0]}: {result.get('error', code)}")
                return result
            except (OSError, TimeoutError, urllib.error.URLError):
                self.nonce = b""
                if not retry or attempt == 2:
                    raise
                time.sleep(0.3 * (attempt + 1))
        raise UpdateError("No authenticated response")

    def status(self):
        status = self.call("status")
        if status.get("uid", "").upper() != self.uid:
            raise AuthenticationError("Authenticated UID mismatch")
        return status

def discover(seconds: float = 3, destinations: tuple[str, ...] = ()) -> list[dict]:
    """Hints only. Every selected device is authenticated again before any write."""
    found = {}
    targets = ("255.255.255.255", "192.168.2.255", *destinations)
    deadline = time.monotonic() + seconds
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("0.0.0.0", 0))
        sock.settimeout(0.4)
        while time.monotonic() < deadline:
            for target in targets:
                try:
                    sock.sendto(b"G100_UPDATE_DISCOVER_V2".ljust(256, b"\0"),
                                (str(ipaddress.IPv4Address(target)), 37020))
                except OSError:
                    continue
            until = min(deadline, time.monotonic() + 1)
            while time.monotonic() < until:
                try:
                    raw, address = sock.recvfrom(4096)
                    value = json.loads(raw)
                    uid = value.get("uid", "").upper()
                    if value.get("protocol") == 2 and len(uid) == 24 and all(c in "0123456789ABCDEF" for c in uid):
                        found[(uid, address[0])] = {"uid": uid, "ip": address[0],
                            "version": str(value.get("firmware", ""))[:80]}
                except (OSError, ValueError, AttributeError):
                    break
    return list(found.values())

def connect(uid: str, key: bytes, ip: str = "", seconds: float = 60) -> Client:
    deadline = time.monotonic() + seconds
    preferred_ip = ip
    last_error = "No board answered discovery"
    while time.monotonic() < deadline:
        candidates = [{"uid": uid, "ip": ip}] if ip else []
        if not candidates:
            candidates = [x for x in discover(min(2, max(.1, deadline-time.monotonic()))) if x["uid"] == uid]
        if len({x["ip"] for x in candidates}) > 1:
            raise UpdateError("The same UID answered from multiple IPs; resolve the duplicate before updating")
        for candidate in candidates:
            try:
                client = Client(candidate["ip"], uid, key, timeout=3)
                client.status()
                client.timeout = 20
                return client
            except AuthenticationError:
                raise
            except (OSError, ValueError, UpdateError) as error:
                last_error = str(error)
        # Alternate discovery with the known address. Routed/VPN peers cannot
        # answer broadcasts, and their first request can race a reboot.
        ip = "" if ip else preferred_ip
        time.sleep(.3)
    raise UpdateError(f"Ethernet connection unavailable: {last_error}. Check power, DHCP, routing and cable; SWD stays closed.")


def verify_installed(uid: str, key: bytes, payload: bytes, version: str, *, seconds=75) -> dict:
    """Read-only acceptance after a complete SWD install, using the new authenticated API."""
    deadline = time.monotonic() + seconds
    digest = hashlib.sha256(payload).hexdigest()
    seen_application = False
    last_error = "Application did not become ready before the deadline"
    while time.monotonic() < deadline:
        try:
            peer = connect(uid, key, seconds=min(12, max(.1, deadline-time.monotonic())))
            state = peer.status()
            if state.get("role") == "application":
                seen_application = True
                if state.get("running_sha256") != digest or state.get("version") != version:
                    return {"status": "failed", "ip": peer.ip, "error": "Running image differs from the installed release"}
                if state.get("healthy") is True and state.get("boot_confirmed") is True:
                    return {"status": "passed", "ip": peer.ip, "sha256": digest, "version": version,
                            "runtime": state, "authenticated": True}
        except AuthenticationError:
            raise
        except (OSError, ValueError, UpdateError) as exc:
            last_error = str(exc)
        time.sleep(.5)
    return {"status": "failed" if seen_application else "unreachable", "error": last_error}

def install(client: Client, payload: bytes, version: str, *, progress=None, reconnect=connect) -> dict:
    started = time.monotonic()
    status = client.status()
    digest = hashlib.sha256(payload).hexdigest()
    if (status.get("role") == "application" and status.get("running_sha256") == digest
            and status.get("version") == version and status.get("boot_confirmed") is True
            and status.get("healthy") is True):
        return {"status": "passed", "already_current": True, "ip": client.ip, "sha256": digest,
                "transport": "ethernet", "swd_used": False}
    if status.get("trial_slot", 0xFFFFFFFF) != 0xFFFFFFFF:
        raise UpdateError("A trial is pending. Allow it to boot/roll back before retrying this installation.")
    if status.get("upload_state") in (1, 2, 4):
        client.call("abort")
    generation = status["generation_floor"] + 1
    image = pack(payload, generation)
    begin = client.call("begin?uid=" + client.uid, image[:128])
    chunk_bytes = begin["chunk_bytes"]
    if type(chunk_bytes) is not int or not 256 <= chunk_bytes <= 16384 or chunk_bytes % 256:
        raise UpdateError("Invalid advertised transfer size")
    transfer_start = time.monotonic()
    for offset in range(0, len(payload), chunk_bytes):
        chunk = payload[offset:offset+chunk_bytes]
        result = client.call(f"chunk?offset={offset}", chunk)
        if result.get("received") != offset + len(chunk):
            raise UpdateError("Board acknowledged an unexpected offset")
        if progress:
            elapsed = max(.001, time.monotonic() - transfer_start)
            progress(result["received"], len(payload), result["received"] / elapsed)
    transfer_seconds = time.monotonic() - transfer_start
    client.call("finish")
    try:
        client.call("activate", retry=False)
    except (OSError, TimeoutError, urllib.error.URLError):
        pass  # A reset may race the response. Only authenticated runtime proof can pass.
    deadline = time.monotonic() + 110
    final = None
    while time.monotonic() < deadline:
        time.sleep(.5)
        try:
            peer = reconnect(client.uid, client.key, client.ip, seconds=min(12, max(.1, deadline-time.monotonic())))
            current = peer.status()
        except AuthenticationError:
            raise
        except (OSError, ValueError, UpdateError):
            continue
        if current.get("role") != "application" or current.get("running_generation") != generation:
            continue
        if current.get("running_sha256") != digest or current.get("version") != version:
            raise UpdateError("Running firmware identity differs from the candidate; trial was not confirmed")
        if current.get("healthy") is not True:
            continue
        final = peer.call("confirm", bytes.fromhex(digest))
        if (final.get("boot_confirmed") is not True or final.get("confirmed_generation") != generation
                or final.get("trial_slot") != 0xFFFFFFFF):
            raise UpdateError("Boot confirmation did not persist")
        break
    if final is None:
        raise UpdateError("Candidate did not pass authenticated runtime checks; rollback remains enabled")
    return {"status": "passed", "ip": peer.ip, "sha256": digest, "generation": generation,
            "version": version, "bytes": len(payload), "transport": "ethernet", "swd_used": False,
            "transfer_seconds": round(transfer_seconds, 3),
            "transfer_bytes_per_second": round(len(payload)/max(.001, transfer_seconds)),
            "elapsed_seconds": round(time.monotonic()-started, 3), "runtime": final}
