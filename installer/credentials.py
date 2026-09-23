"""Per-user Windows DPAPI storage. Keys never appear in UI state or run reports."""
from __future__ import annotations
import ctypes as ct
from ctypes import wintypes
import json
from pathlib import Path
import re
import secrets
import struct
import sys
import threading
import zlib
import release
from state_lock import file_lock

_lock = threading.RLock()

def checked_uid(uid: str) -> str:
    if not isinstance(uid, str) or not re.fullmatch(r"[0-9A-Fa-f]{24}", uid):
        raise ValueError("A full 96-bit board UID is required")
    return uid.upper()

def _protect(data: bytes, decrypt: bool = False) -> bytes:
    if sys.platform != "win32":
        raise RuntimeError("Credential storage requires Windows DPAPI")
    class Blob(ct.Structure):
        _fields_ = [("size", wintypes.DWORD), ("data", ct.POINTER(ct.c_ubyte))]
    buf = (ct.c_ubyte * len(data)).from_buffer_copy(data)
    source, target = Blob(len(data), buf), Blob()
    crypt = ct.WinDLL("crypt32", use_last_error=True)
    kernel = ct.WinDLL("kernel32", use_last_error=True)
    function = crypt.CryptUnprotectData if decrypt else crypt.CryptProtectData
    function.argtypes = [ct.POINTER(Blob), ct.c_void_p, ct.c_void_p, ct.c_void_p,
                         ct.c_void_p, wintypes.DWORD, ct.POINTER(Blob)]
    function.restype = wintypes.BOOL
    kernel.LocalFree.argtypes = [ct.c_void_p]
    kernel.LocalFree.restype = ct.c_void_p
    if not function(ct.byref(source), None, None, None, None, 1, ct.byref(target)):
        raise ct.WinError(ct.get_last_error())
    try:
        return ct.string_at(target.data, target.size)
    finally:
        kernel.LocalFree(target.data)

def store_key(uid: str, key: bytes) -> None:
    uid = checked_uid(uid)
    if len(key) != 32:
        raise ValueError("The update key must contain 32 bytes")
    folder = release.STATE_ROOT / "credentials"
    folder.mkdir(parents=True, exist_ok=True)
    with _lock, file_lock("credential-" + uid, wait=5):
        temp = folder / (uid + ".partial")
        temp.write_bytes(_protect(key))
        temp.replace(folder / (uid + ".dpapi"))

def load_key(uid: str) -> bytes:
    key = _protect((release.STATE_ROOT / "credentials" / (checked_uid(uid) + ".dpapi")).read_bytes(), True)
    if len(key) != 32:
        raise ValueError("Invalid stored update key")
    return key

def ensure_key(uid: str) -> bytes:
    with _lock:
        path = release.STATE_ROOT / "credentials" / (checked_uid(uid) + ".dpapi")
        if path.exists():
            return load_key(uid)
        key = secrets.token_bytes(32)
        store_key(uid, key)
        if load_key(uid) != key:
            raise ValueError("Credential readback failed")
        return key

def provision_record(uid: str, key: bytes) -> bytes:
    uid = checked_uid(uid)
    if len(key) != 32:
        raise ValueError("Invalid key length")
    raw = struct.pack("<5I", 0x324B5547, 1, *(int(uid[i:i+8], 16) for i in (0, 8, 16)))
    raw += key + bytes(8)
    return raw + struct.pack("<I", zlib.crc32(raw))

def _read_fleet() -> list[dict]:
    path = release.STATE_ROOT / "fleet.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else []

def fleet() -> list[dict]:
    with _lock, file_lock("registry", wait=5):
        return _read_fleet()

def remember(uid: str, **values) -> dict:
    uid = checked_uid(uid)
    allowed = {"ip", "label", "auto_update", "last_release", "last_version", "last_error"}
    if not set(values) <= allowed:
        raise ValueError("Invalid fleet field")
    with _lock, file_lock("registry", wait=5):
        items = {item["uid"]: item for item in _read_fleet()}
        entry = items.setdefault(uid, {"uid": uid, "label": uid, "ip": "", "auto_update": False})
        entry.update(values)
        release.STATE_ROOT.mkdir(parents=True, exist_ok=True)
        temp = release.STATE_ROOT / "fleet.partial"
        temp.write_text(json.dumps(list(items.values()), indent=2), encoding="utf-8")
        temp.replace(release.STATE_ROOT / "fleet.json")
        return entry.copy()
