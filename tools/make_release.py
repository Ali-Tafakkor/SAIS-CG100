"""Create firmware, independent recovery, Stage 0 and release manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "installer"))
from image_format import admit, FACTORY_BYTES, HEADER  # noqa: E402


def make(tag: str, output: Path) -> dict:
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?", tag):
        raise ValueError("Release tag must look like v0.1.0")
    app = ROOT / "SetAPIs_Calling/firmware/build-shadow/g100-api.bin"
    status = json.loads((app.parent / "build-status.json").read_text(encoding="utf-8-sig"))
    stage0 = ROOT / "firmware/bootloader/build/stage0.bin"
    stage_status = json.loads((stage0.parent / "stage0-build.json").read_text(encoding="utf-8-sig"))
    recovery = ROOT / "SetAPIs_Calling/firmware/build-recovery/g100-api.bin"
    recovery_status = json.loads((recovery.parent / "build-status.json").read_text(encoding="utf-8-sig"))
    files = {}
    for name, source in (("g100-api.bin", app), ("stage0.bin", stage0), ("recovery.bin", recovery)):
        if not source.is_file():
            raise ValueError(f"Build output missing: {source}")
        data = source.read_bytes()
        files[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if (status.get("success") is not True or status.get("profile") != "shadow" or
            status.get("sha256", "").lower() != files["g100-api.bin"]["sha256"]):
        raise ValueError("Application build status does not match the binary")
    if (stage_status.get("bytes") != files["stage0.bin"]["bytes"] or
            stage_status.get("sha256") != files["stage0.bin"]["sha256"]):
        raise ValueError("Stage 0 build status does not match the binary")
    if (recovery_status.get("success") is not True or recovery_status.get("profile") != "recovery" or
            recovery_status.get("sha256", "").lower() != files["recovery.bin"]["sha256"]):
        raise ValueError("Recovery build status does not match the binary")
    admit(app.read_bytes())
    admit(recovery.read_bytes(), FACTORY_BYTES - HEADER)
    if not 1024 < stage0.stat().st_size < 0x20000:
        raise ValueError("Stage 0 exceeds internal Flash")
    config = (ROOT / "SetAPIs_Calling/firmware/app/device_config.h").read_text()
    match = re.search(r'#elif defined\(G100_SHADOW\)\s*#define G100_FIRMWARE_VERSION\s+"([^"]+)"', config)
    if not match:
        raise ValueError("Shadow firmware version is missing")
    version = match.group(1)
    if version.encode("ascii") not in app.read_bytes():
        raise ValueError("Application version differs from the built binary")
    commit = subprocess.check_output(["git", "-c", "safe.directory=" + ROOT.as_posix(), "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "-c", "safe.directory=" + ROOT.as_posix(),
        "status", "--porcelain", "--untracked-files=normal"], cwd=ROOT, text=True).strip())
    manifest = {"schema": 2, "update_protocol": 2, "recovery_version": "1.0.0-recovery", "release_tag": tag, "source_commit": commit,
                "source_dirty": dirty,
                "hardware": "MainBoard-v2.6-H750", "architecture": "nor-sdram-shadow-v1",
                "firmware_version": version, "files": files}
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(app, output / "g100-api.bin")
    shutil.copy2(stage0, output / "stage0.bin")
    shutil.copy2(recovery, output / "recovery.bin")
    (output / "firmware-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    print(json.dumps(make(arguments.tag, arguments.output), indent=2))
