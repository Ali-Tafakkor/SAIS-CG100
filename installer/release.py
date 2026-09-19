"""Fetch and validate immutable firmware assets from the latest GitHub release."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.request

REPO = "Ali-Tafakkor/SAIS-CG100"
RELEASES = f"https://github.com/{REPO}/releases"
LATEST_MANIFEST = f"{RELEASES}/latest/download/firmware-manifest.json"
STATE_ROOT = Path(os.environ.get("G100_STATE_ROOT") or
                  (Path(os.environ.get("LOCALAPPDATA", Path.home())) / "SAIS-CG100"))
MAX_APP = 0xD0000 - 4096
TAG_PATTERN = r"v[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?"


class ReleaseError(RuntimeError):
    pass


def _urllib_fetch(url: str, maximum: int) -> bytes:
    request = urllib.request.Request(url, headers={
        "Accept": "application/octet-stream", "User-Agent": "SAIS-CG100-Programmer/1"})
    with urllib.request.urlopen(request, timeout=15) as response:
        if not response.geturl().startswith("https://"):
            raise ReleaseError("A release download redirected away from HTTPS")
        content = response.read(maximum + 1)
    if len(content) > maximum:
        raise ReleaseError("Release asset exceeds its size limit")
    return content


def _windows_fetch(url: str, maximum: int, transport: str) -> bytes:
    cache = STATE_ROOT / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as temporary:
        output = Path(temporary) / "asset"
        if transport == "Windows curl":
            executable = shutil.which("curl.exe")
            if not executable:
                raise OSError("curl.exe is unavailable")
            command = [executable, "--location", "--fail", "--silent", "--show-error",
                       "--proto-redir", "=https", "--connect-timeout", "10",
                       "--max-time", "45", "--max-filesize", str(maximum),
                       "--output", str(output), url]
        else:
            executable = shutil.which("powershell.exe")
            script = Path(__file__).with_name("download.ps1")
            if not executable or not script.is_file():
                raise OSError("PowerShell downloader is unavailable")
            command = [executable, "-NoProfile", "-NonInteractive", "-ExecutionPolicy",
                       "Bypass", "-File", str(script), "-Url", url,
                       "-Output", str(output), "-MaximumBytes", str(maximum)]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=55)
        except subprocess.TimeoutExpired as exc:
            raise OSError(f"{transport} timed out") from exc
        if result.returncode or not output.is_file():
            detail = (result.stderr or result.stdout or "download did not create a file").strip()
            raise OSError(f"{transport} failed: {detail[:180]}")
        if output.stat().st_size > maximum:
            raise ReleaseError("Release asset exceeds its size limit")
        return output.read_bytes()


def _fetch(url: str, maximum: int = 8 * 1024 * 1024) -> bytes:
    if not url.startswith("https://"):
        raise ReleaseError("Only HTTPS release downloads are permitted")
    errors = []
    try:
        return _urllib_fetch(url, maximum)
    except urllib.error.HTTPError as exc:
        if exc.code not in (408, 429, 500, 502, 503, 504):
            raise ReleaseError(f"GitHub returned HTTP {exc.code} for a release asset") from exc
        errors.append(f"Python HTTPS: HTTP {exc.code}")
    except (OSError, TimeoutError) as exc:
        errors.append(f"Python HTTPS: {str(exc)[:180]}")
    for transport in ("Windows curl", "PowerShell"):
        try:
            return _windows_fetch(url, maximum, transport)
        except OSError as exc:
            errors.append(f"{transport}: {str(exc)[:180]}")
    raise ReleaseError("Could not download firmware from GitHub. Check access to github.com "
                       "and release-assets.githubusercontent.com, then retry. " +
                       "; ".join(errors))


def latest() -> dict:
    try:
        manifest = json.loads(_fetch(LATEST_MANIFEST, 64 * 1024))
    except (ValueError, TypeError) as exc:
        raise ReleaseError("Latest firmware manifest is not valid JSON") from exc
    if not isinstance(manifest, dict):
        raise ReleaseError("Latest firmware manifest must be an object")
    tag = manifest.get("release_tag", "")
    if not isinstance(tag, str) or not re.fullmatch(TAG_PATTERN, tag):
        raise ReleaseError("The latest release tag is invalid")
    assets = {name: f"{RELEASES}/download/{tag}/{name}"
              for name in ("firmware-manifest.json", "g100-api.bin", "stage0.bin")}
    return {"tag": tag, "url": f"{RELEASES}/tag/{tag}", "assets": assets,
            "manifest": manifest}


def fetch_bundle(release: dict | None = None) -> dict:
    release = release or latest()
    tag = release["tag"]
    folder = STATE_ROOT / "cache" / tag
    folder.mkdir(parents=True, exist_ok=True)
    manifest = release.get("manifest")
    if manifest is None:
        try:
            manifest = json.loads(_fetch(release["assets"]["firmware-manifest.json"], 64 * 1024))
        except (ValueError, TypeError) as exc:
            raise ReleaseError("Firmware manifest is not valid JSON") from exc
    if not isinstance(manifest, dict):
        raise ReleaseError("Firmware manifest must be an object")
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
