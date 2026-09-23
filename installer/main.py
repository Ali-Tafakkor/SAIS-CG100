"""Local browser UI for the portable SAIS-CG100 Windows programmer."""

from __future__ import annotations

import argparse
import base64
import struct
import zlib
import credentials
import fleet_agent
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import threading
import launcher
from operation_lock import board_lock

from controller import Controller

WEB = Path(__file__).resolve().parent / "web"
CONTROL = Controller()
TOKEN = secrets.token_urlsafe(32)


class Handler(BaseHTTPRequestHandler):
    def _send(self, code: int, content: bytes, mime: str,
              extra_headers: dict[str, str] | None = None):
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self'; script-src 'self'; connect-src 'self'; object-src 'none'")
        for name, value in (extra_headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(content)

    def _json(self, code: int, data: dict):
        self._send(code, json.dumps(data).encode("utf-8"), "application/json; charset=utf-8")

    def do_GET(self):
        if self.path == "/api/state":
            try:
                state = CONTROL.state()
            except Exception as exc:
                launcher.LOG.exception("Could not read installer state")
                self._json(503, {"error": f"Cannot read installer state: {exc}. See launcher.log."})
                return
            self._json(200, {"state": state, "token": TOKEN})
            launcher.UI_READY.set()
            return
        if self.path == "/api/report":
            path = CONTROL.report_path
            if not path or not Path(path).is_file():
                self._json(404, {"error": "No report is available yet"})
                return
            self._send(200, Path(path).read_bytes(), "application/json; charset=utf-8",
                       {"Content-Disposition": "attachment; filename=SAIS-CG100-report.json"})
            return
        files = {"/": ("index.html", "text/html; charset=utf-8"),
                 "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                 "/style.css": ("style.css", "text/css; charset=utf-8")}
        if self.path not in files:
            self._json(404, {"error": "Not found"})
            return
        name, mime = files[self.path]
        self._send(200, (WEB / name).read_bytes(), mime)

    def do_POST(self):
        origin = self.headers.get("Origin")
        expected_origin = f"http://127.0.0.1:{self.server.server_port}"
        if (self.headers.get("X-G100-Token") != TOKEN or
                (origin and origin != expected_origin)):
            self._json(403, {"error": "Invalid local-session token or origin"})
            return
        if self.headers.get("Content-Type", "").split(";", 1)[0] != "application/json":
            self._json(415, {"error": "JSON request required"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 <= length <= 16384:
                raise ValueError("Invalid request length")
            request = json.loads(self.rfile.read(length))
            if not isinstance(request, dict):
                raise ValueError("JSON object required")
            if self.path == "/api/scan":
                result = {"probes": CONTROL.scan()}
            elif self.path == "/api/release":
                result = {"release": CONTROL.check_release()}
            elif self.path == "/api/included":
                result = {"release": CONTROL.load_included()}
            elif self.path == "/api/network/scan":
                result = {"boards": CONTROL.scan_network(request.get("address", ""))}
            elif self.path == "/api/fleet/resume":
                if CONTROL.running:
                    raise ValueError("Wait for the current operation to finish")
                fleet_agent.clear_holds()
                result = {"ok": True}
            elif self.path == "/api/fleet/options":
                if CONTROL.running:
                    raise ValueError("Wait for the current operation to finish")
                uid = credentials.checked_uid(request.get("uid", ""))
                credentials.load_key(uid)
                automatic = request.get("auto_update", False)
                if type(automatic) is not bool:
                    raise ValueError("Automatic update preference must be true or false")
                result = {"board": credentials.remember(uid, auto_update=automatic)}
            elif self.path == "/api/fleet/import":
                if CONTROL.running:
                    raise ValueError("Wait for the current operation to finish")
                raw = base64.b64decode(request.get("record", ""), validate=True)
                if (len(raw) != 64 or struct.unpack_from("<II", raw) != (0x324B5547, 1)
                        or zlib.crc32(raw[:60]) != struct.unpack_from("<I", raw, 60)[0]):
                    raise ValueError("Choose the board-access.g100-key file saved during initial installation")
                uid = "".join(f"{n:08X}" for n in struct.unpack_from("<3I", raw, 8))
                # Another installer or the unattended worker may own this board.
                # Do not replace its key midway through an authenticated update.
                with board_lock(uid):
                    credentials.store_key(uid, raw[20:52])
                    result = {"board": credentials.remember(uid)}
            elif self.path == "/api/inspect":
                result = {"inspected": CONTROL.inspect(request.get("serials", []))}
            elif self.path == "/api/start":
                run_id = CONTROL.start(request.get("mode", ""),
                                       request.get("serials", []),
                                       request.get("confirmation", ""),
                                       request.get("max_parallel", 2),
                                       request.get("release_tag"))
                result = {"run_id": run_id}
            else:
                self._json(404, {"error": "Unknown action"})
                return
            self._json(200, result)
        except Exception as exc:
            self._json(400, {"error": str(exc)})

    def log_message(self, format, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    launcher.prepare()
    if (WEB.parent.parent / "firmware-release" / "firmware-manifest.json").is_file():
        CONTROL.load_included()
    with ThreadingHTTPServer(("127.0.0.1", args.port), Handler) as server:
        url = f"http://127.0.0.1:{server.server_port}/"
        launcher.serve_notice(url, not args.no_browser)
        server.serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        launcher.LOG.exception("Installer startup failed")
        print(f"Installer could not start: {exc}", flush=True)
        raise SystemExit(1)
