"""Fetch and validate immutable firmware assets from the latest GitHub release."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import urllib.request

REPO = "Ali-Tafakkor/SAIS-CG100"
API = f"https://api.github.com/repos/{REPO}/releases/latest"
STATE_ROOT = Path(os.environ.get("G100_STATE_ROOT") or
                  (Path(os.environ.get("LOCALAPPDATA", Path.home())) / "SAIS-CG100"))
MAX_APP = 0xD0000 - 4096


class ReleaseError(RuntimeError):
    pass


def _fetch(url: str, maximum: int = 8 * 1024 * 1024) -> bytes:
    if not url.startswith("https://"):
        raise ReleaseError("Only HTTPS release downloads are permitted")
    request = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json", "User-Agent": "SAIS-CG100-Programmer/1"})
    with urllib.request.urlopen(request, timeout=45) as response:
        content = response.read(maximum + 1)
    if len(content) > maximum:
        raise ReleaseError("Release asset exceeds its size limit")
    return content


def latest() -> dict:
    raw = json.loads(_fetch(API, 256 * 1024))
    tag = raw.get("tag_name", "")
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?", tag):
        raise ReleaseError("The latest release tag is invalid")
    assets = {a["name"]: a["browser_download_url"] for a in raw.get("assets", [])
              if isinstance(a.get("name"), str) and isinstance(a.get("browser_download_url"), str)}
    for name in ("firmware-manifest.json", "g100-api.bin", "stage0.bin"):
        if name not in assets:
            raise ReleaseError(f"Latest release {tag} is missing {name}")
    return {"tag": tag, "url": raw.get("html_url", ""), "assets": assets}


def fetch_bundle(release: dict | None = None) -> dict:
    release = release or latest()
    tag = release["tag"]
    folder = STATE_ROOT / "cache" / tag
    folder.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(_fetch(release["assets"]["firmware-manifest.json"], 64 * 1024))
    if (manifest.get("schema") != 1 or manifest.get("release_tag") != tag or
            manifest.get("hardware") != "MainBoard-v2.6-H750" or
            manifest.get("architecture") != "nor-sdram-shadow-v1"):
        raise ReleaseError("Firmware manifest does not match the selected release/hardware")
    expected_files = {"g100-api.bin": (1024, MAX_APP), "stage0.bin": (1024, 0x20000)}
    fetched = {}
    for name, (minimum, maximum) in expected_files.items():
        spec = manifest.get("files", {}).get(name, {})
        length, digest = spec.get("bytes"), spec.get("sha256", "")
        if not isinstance(length, int) or not minimum < length <= maximum or not re.fullmatch(
                r"[0-9a-f]{64}", digest):
            raise ReleaseError(f"Invalid {name} entry in the release manifest")
        path = folder / name
        if not path.is_file() or path.stat().st_size != length or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            content = _fetch(release["assets"][name], maximum)
            if len(content) != length or hashlib.sha256(content).hexdigest() != digest:
                raise ReleaseError(f"Downloaded {name} failed its SHA-256/length check")
            temporary = path.with_suffix(path.suffix + ".partial")
            temporary.write_bytes(content)
            temporary.replace(path)
        fetched[name] = path
    from image_format import admit
    admit(fetched["g100-api.bin"].read_bytes(), MAX_APP)
    (folder / "firmware-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return {"release": release, "manifest": manifest, "files": fetched}
