"""Launch the real packaged CMD from another directory and verify its local UI.

No ST-LINK enumeration or board request is performed. The launched process tree
is stopped after read-only localhost checks; the user's installer is untouched.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def test_package(package: Path, entry: Path | None = None) -> dict:
    package = package.resolve()
    entry = (entry or package / "Run-Programmer.cmd").resolve()
    if any(char in str(entry) for char in ('"', '%', '\r', '\n')):
        raise ValueError("Unsupported CMD test path")
    manifest = json.loads((package / "firmware-release/firmware-manifest.json").read_text())
    with tempfile.TemporaryDirectory(prefix="G100 launcher test ") as temporary:
        state = Path(temporary) / "state"
        environment = {**os.environ, "G100_STATE_ROOT": str(state),
                       "G100_OPENOCD_ROOT": str(Path(temporary) / "no-probe-runtime")}
        command = f'"{os.environ["COMSPEC"]}" /d /s /c ""{entry}" --no-browser --port 0"'
        process = subprocess.Popen(command, cwd=temporary, env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            creationflags=subprocess.CREATE_NO_WINDOW)
        lines = queue.Queue()
        def read_output():
            for line in process.stdout:
                lines.put(line.rstrip())
        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        output, url = [], None
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=1)
                    output.append(line)
                    if line.startswith("SAIS-CG100 Programmer: http://127.0.0.1:"):
                        url = line.split(": ", 1)[1].rstrip("/")
                        break
                except queue.Empty:
                    if process.poll() is not None:
                        break
            if not url:
                raise AssertionError("CMD did not start the UI: " + "\n".join(output))
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            for path in ("/", "/app.js", "/style.css"):
                with opener.open(url+path, timeout=5) as response:
                    assert response.status == 200 and len(response.read()) > 100
            with opener.open(url+"/api/state", timeout=5) as response:
                current = json.load(response)["state"]
            assert current["release"]["tag"] == manifest["release_tag"]
            assert current["release"]["source"] == "included"
            assert current["release"]["network_supported"] is True
            assert not current["running"] and not current["probes"] and not current["network_boards"]
            assert url in (state / "Open-Installer.url").read_text()
            assert (state / "launcher.log").is_file()
            assert not (state / "reports").exists()
            assert not (state / "credentials").exists()
            result = {"status": "passed", "entry": str(entry), "package": str(package),
                      "release": current["release"], "actual_cmd_executed": True,
                      "different_working_directory": True, "local_ui_and_state": "passed",
                      "automatic_browser": "disabled for this automated test",
                      "board_operations": "none"}
        finally:
            # PID comes only from the child created above, never from enumeration.
            if process.poll() is None:
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
                    creationflags=subprocess.CREATE_NO_WINDOW, timeout=10)
            process.wait(timeout=5)
            reader.join(timeout=2)
            process.stdout.close()
        return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--entry", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    package = args.package or ROOT / "dist" / (ROOT / "dist/latest-package.txt").read_text().strip()
    result = test_package(package, args.entry)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
