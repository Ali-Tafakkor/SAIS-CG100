"""Parallel, per-probe provisioning with explicit pre-erase confirmation."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
import datetime as dt
import json
from pathlib import Path
import re
import shutil
import threading
import time
import uuid

import hardware
import network
import release
import credentials
import ethernet
from operation_lock import board_lock

PHASE_SECONDS = {"backup": 540, "erase": 240, "blank check": 300,
                 "program": 180, "readback": 300, "Ethernet tests": 45}


class Controller:
    def __init__(self):
        self.lock = threading.RLock()
        self.probes: list[dict] = []
        self.network_boards: list[dict] = []
        self.inspected: dict[str, dict] = {}
        self.bundle: dict | None = None
        self.release_error = ""
        self.running = False
        self.run_id: str | None = None
        self.mode: str | None = None
        self.boards: dict[str, dict] = {}
        self.general_error = ""
        self.report_path: str | None = None

    def state(self) -> dict:
        with self.lock:
            boards = {serial: board.copy() for serial, board in self.boards.items()}
            now = time.monotonic()
            phases = list(PHASE_SECONDS)
            for board in boards.values():
                phase = board.get("phase")
                if board.get("state") != "running" or phase not in PHASE_SECONDS:
                    continue
                elapsed = max(0, round(now - board.get("phase_started", now)))
                board["elapsed_seconds"] = board.get("elapsed_seconds", 0) + elapsed
                later = phases[phases.index(phase) + 1:]
                if self.mode == "erase-only":
                    later = [] if phase == "blank check" else [p for p in later if p in ("erase", "blank check")]
                board["estimated_remaining_seconds"] = max(0, PHASE_SECONDS[phase] - elapsed) + sum(
                    PHASE_SECONDS[p] for p in later)
                board["estimate_overdue"] = elapsed > PHASE_SECONDS[phase]
            return {"probes": self.probes, "inspected": self.inspected,
                    "release": None if not self.bundle else {
                        "tag": self.bundle["release"]["tag"],
                        "version": self.bundle["manifest"]["firmware_version"],
                        "url": self.bundle["release"]["url"],
                        "network_supported": self.bundle["manifest"].get("update_protocol") == 2,
                        "source": "included" if self.bundle["release"].get("local") else "download"},
                    "network_boards": self.network_boards, "fleet": credentials.fleet(),
                    "release_error": self.release_error,
                    "running": self.running, "run_id": self.run_id,
                    "mode": self.mode, "boards": boards,
                    "general_error": self.general_error, "report_path": self.report_path}

    def scan(self) -> list[dict]:
        with self.lock:
            if self.running:
                raise ValueError("A run is already in progress")
        probes = hardware.list_probes()
        with self.lock:
            self.probes = probes
            self.inspected = {}
        return probes

    def load_included(self) -> dict:
        with self.lock:
            if self.running:
                raise ValueError("A run is already in progress")
        bundle = release.load_local(hardware.ROOT / "firmware-release")
        with self.lock:
            self.bundle, self.release_error = bundle, ""
        return {"tag": bundle["release"]["tag"], "version": bundle["manifest"]["firmware_version"]}

    def scan_network(self, address: str = "") -> list[dict]:
        with self.lock:
            if self.running:
                raise ValueError("A run is already in progress")
        import ipaddress
        if address:
            address = str(ipaddress.IPv4Address(address))
        known = {entry["uid"]: entry for entry in credentials.fleet()}
        discovered = ethernet.discover(3, (address,) if address else ())
        if address:
            try:
                hint = network._get_json(address, "/api/v1/firmware/status")
                uid = credentials.checked_uid(hint.get("uid", ""))
                discovered.append({"uid": uid, "ip": address, "version": hint.get("version", "")})
            except (OSError, ValueError):
                pass
        results = {}
        for entry in discovered:
            uid = entry["uid"]
            if uid in results and results[uid]["ip"] != entry["ip"]:
                results[uid]["error"] = "Duplicate UID at multiple addresses"
                continue
            value = {**known.get(uid, {}), **entry, "online": True, "has_key": False}
            try:
                credentials.load_key(uid)
                value["has_key"] = True
            except (OSError, ValueError):
                value["error"] = "Install the bootstrap once or import this board's access file"
            results[uid] = value
        for uid, entry in known.items():
            if uid not in results:
                results[uid] = {**entry, "online": False, "has_key": True}
        with self.lock:
            self.network_boards = list(results.values())
        return self.network_boards

    def check_release(self) -> dict:
        with self.lock:
            if self.running:
                raise ValueError("A run is already in progress")
        try:
            bundle = release.fetch_bundle()
        except Exception as exc:
            with self.lock:
                self.bundle = None
                self.release_error = str(exc)
            raise
        with self.lock:
            self.bundle = bundle
            self.release_error = ""
        return {"tag": bundle["release"]["tag"],
                "version": bundle["manifest"]["firmware_version"]}

    def inspect(self, serials: list[str]) -> dict:
        with self.lock:
            if self.running:
                raise ValueError("A run is already in progress")
            available = {p["serial"] for p in self.probes}
        if not serials or len(serials) != len(set(serials)) or not set(serials) <= available:
            raise ValueError("Select one or more discovered ST-LINK probes")
        results: dict[str, dict] = {}
        base = release.STATE_ROOT / "inspections" / uuid.uuid4().hex
        with ThreadPoolExecutor(max_workers=min(4, len(serials))) as executor:
            futures = {executor.submit(hardware.inspect, serial, base / serial): serial
                       for serial in serials}
            for future in as_completed(futures):
                serial = futures[future]
                try:
                    results[serial] = future.result()
                except Exception as exc:
                    results[serial] = {"probe_serial": serial, "error": str(exc)}
        uids = [item["uid"] for item in results.values() if "uid" in item]
        if len(uids) != len(set(uids)):
            for item in results.values():
                item["error"] = "The same MCU UID appeared on multiple probes"
        with self.lock:
            self.inspected = results
        return results

    def start(self, mode: str, serials: list[str], confirmation: str,
              max_parallel: int, release_tag: str | None = None) -> str:
        if mode not in ("install", "network-install", "network-update", "erase-only"):
            raise ValueError("Choose SWD install, Ethernet install, Ethernet update or erase-only")
        if not serials or len(serials) != len(set(serials)):
            raise ValueError("Select distinct inspected boards")
        verb = "UPDATE" if mode == "network-update" else "ERASE"
        if confirmation != f"{verb} {len(serials)}":
            raise ValueError(f"Type {verb} {len(serials)} to confirm this operation")
        if not 1 <= max_parallel <= 8:
            raise ValueError("Parallel worker count must be 1â€“8")
        with self.lock:
            if self.running:
                raise ValueError("A run is already in progress")
            if mode == "network-update":
                available = {item["uid"]: item for item in self.network_boards}
                if not set(serials) <= available.keys():
                    raise ValueError("Discover or register every selected network board first")
                selected = [{**available[uid], "probe_serial": uid} for uid in serials]
                for item in selected:
                    credentials.load_key(item["uid"])
            else:
                if not set(serials) <= self.inspected.keys():
                    raise ValueError("Inspect every selected board first")
                selected = [self.inspected[s].copy() for s in serials]
            if any("uid" not in item or "error" in item for item in selected):
                raise ValueError("Every selected board must pass inspection")
            if len({item["uid"] for item in selected}) != len(selected):
                raise ValueError("Selected MCU UIDs must be unique")
            if mode != "erase-only" and (not self.bundle or self.bundle["release"]["tag"] != release_tag):
                raise ValueError("Fetch or load and review a firmware release first")
            if mode in ("network-install", "network-update") and self.bundle["manifest"].get("update_protocol") != 2:
                raise ValueError("This release lacks the Ethernet bootstrap. Load a protocol-v2 package.")
            bundle = self.bundle
            self.run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8]
            self.mode = mode
            self.running = True
            self.general_error = ""
            self.report_path = None
            self.boards = {item["probe_serial"]: {
                "uid": item["uid"], "probe_serial": item["probe_serial"],
                "state": "queued", "phase": "queued", "percent": 0,
                "started_at": None, "elapsed_seconds": 0,
                "estimated_remaining_seconds": sum(PHASE_SECONDS.values()),
                "checks": [], "error": ""} for item in selected}
            run_id = self.run_id
        thread = threading.Thread(target=self._run_batch,
                                  args=(run_id, mode, selected, bundle, max_parallel),
                                  daemon=True)
        thread.start()
        return run_id

    def _update(self, serial: str, **values):
        with self.lock:
            self.boards[serial].update(values)

    def _phase(self, serial: str, phase: str, percent: int, begun: float):
        self._update(serial, state="running", phase=phase, percent=percent,
                     phase_started=time.monotonic(),
                     elapsed_seconds=round(time.monotonic() - begun),
                     estimated_remaining_seconds=sum(PHASE_SECONDS.values()) * (100 - percent) // 100)

    def _run_board(self, run_dir: Path, mode: str, item: dict, bundle: dict | None) -> dict:
        try:
            with board_lock(item["uid"]):
                return self._run_board_locked(run_dir, mode, item, bundle)
        except Exception as exc:
            self._update(item["probe_serial"], state="failed", phase="stopped", error=str(exc))
            return {"uid": item["uid"], "mode": mode, "status": "failed", "error": str(exc)}

    def _network_install(self, serial, uid, bundle, begun, ip=""):
        key = credentials.load_key(uid)
        self._phase(serial, "Find board on Ethernet", 72, begun)
        peer = ethernet.connect(uid, key, ip, seconds=75)
        app = bundle["files"]["g100-api.bin"].read_bytes()
        def progress(done, total, speed):
            self._update(serial, phase="Transfer over Ethernet", percent=75 + int(20*done/total),
                transferred_bytes=done, total_bytes=total, bytes_per_second=round(speed),
                estimated_remaining_seconds=round((total-done)/max(1,speed)),
                elapsed_seconds=round(time.monotonic()-begun))
        result = ethernet.install(peer, app, bundle["manifest"]["firmware_version"], progress=progress)
        credentials.remember(uid, ip=result["ip"], last_release=bundle["release"]["tag"],
            last_version=bundle["manifest"]["firmware_version"], last_error="")
        return result

    def _run_board_locked(self, run_dir: Path, mode: str, item: dict, bundle: dict | None) -> dict:
        serial, uid = item["probe_serial"], item["uid"]
        directory = run_dir / f"board-{uid}"
        directory.mkdir(parents=True, exist_ok=False)
        begun = time.monotonic()
        record = {"uid": uid, "probe_serial": serial, "mode": mode,
                  "pre_erase_inspection": item, "checks": [], "status": "failed"}
        self._update(serial, started_at=dt.datetime.now(dt.timezone.utc).isoformat())
        try:
            if mode == "network-update":
                record["network"] = self._network_install(serial, uid, bundle, begun, item.get("ip", ""))
                record["status"] = "passed"
                record["checks"].append("Authenticated Ethernet-only transfer, runtime and boot confirmation")
                self._update(serial, state="passed", phase="complete", percent=100, estimated_remaining_seconds=0)
                return record
            # Persist and read back access credentials BEFORE any erase.
            key = credentials.ensure_key(uid) if bundle and "recovery.bin" in bundle["files"] else None
            self._phase(serial, "backup", 5, begun)
            available = shutil.disk_usage(directory).free
            if available < 110 * 1024 * 1024:
                raise hardware.HardwareError("At least 110 MiB of free disk space is required per board")
            record["backup"] = hardware.full_backup(serial, uid, directory)
            record["checks"].append("Two matching full internal/NOR backup reads")
            self._phase(serial, "erase", 30, begun)
            hardware.erase_all(serial, uid, directory)
            self._phase(serial, "blank check", 45, begun)
            blank = hardware.verify_post_write(serial, uid, directory)
            record["blank_readback"] = blank
            record["checks"].append("All 128 KiB internal Flash and 16 MiB NOR read as erased")
            if mode == "erase-only":
                record["status"] = "passed"
                self._update(serial, state="passed", phase="complete", percent=100,
                             estimated_remaining_seconds=0)
                return record
            assert bundle is not None
            self._phase(serial, "program", 60, begun)
            app = bundle["files"]["g100-api.bin"].read_bytes()
            stage0 = bundle["files"]["stage0.bin"]
            recovery = bundle["files"].get("recovery.bin")
            plan = hardware.prepare_install(directory, app, stage0.read_bytes(),
                recovery=recovery.read_bytes() if recovery else None,
                provision=credentials.provision_record(uid, key) if key else None,
                bootstrap_only=mode == "network-install")
            hardware.program(serial, uid, directory, plan, stage0)
            record["swd_closed_at"] = dt.datetime.now(dt.timezone.utc).isoformat()
            if key:
                credentials.remember(uid)
            if mode == "network-install":
                # HARD BOUNDARY: no hardware/SWD call is allowed below this branch.
                record["checks"].append("Recovery, device access record and empty A/B metadata verified; SWD process exited")
                record["network"] = self._network_install(serial, uid, bundle, begun)
                record["status"] = "passed"
                record["checks"].append("Main application installed and confirmed entirely over Ethernet")
                self._update(serial, state="passed", phase="complete", percent=100, estimated_remaining_seconds=0)
                return record
            record["checks"].append("Application, recovery and boot metadata verified before Stage 0")
            self._phase(serial, "readback", 80, begun)
            record["final_readback"] = hardware.verify_post_write(serial, uid, directory, plan)
            record["swd_closed_at"] = dt.datetime.now(dt.timezone.utc).isoformat()
            record["checks"].append("Full internal/NOR readback equals the expected firmware layout")
            self._phase(serial, "Ethernet tests", 94, begun)
            record["network"] = (ethernet.verify_installed(uid, key, app, bundle["manifest"]["firmware_version"])
                if key else network.test_runtime(uid, bundle["manifest"]["firmware_version"], len(app)))
            if key and record["network"]["status"] == "passed":
                credentials.remember(uid, ip=record["network"].get("ip", ""),
                    last_release=bundle["release"]["tag"], last_version=bundle["manifest"]["firmware_version"])
            record["status"] = ("passed" if record["network"]["status"] == "passed"
                                else "partial" if record["network"]["status"] == "unreachable"
                                else "failed")
            if record["network"]["status"] == "passed":
                record["checks"].append("UID, version, health, boot and SDRAM checks passed over Ethernet")
            self._update(serial, state=record["status"], phase="complete", percent=100,
                         estimated_remaining_seconds=0)
        except Exception as exc:
            record["error"] = str(exc)
            self._update(serial, state="failed", phase="stopped", error=str(exc),
                         estimated_remaining_seconds=0)
        finally:
            record["elapsed_seconds"] = round(time.monotonic() - begun)
            self._update(serial, elapsed_seconds=record["elapsed_seconds"])
            (directory / "result.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        return record

    def _run_batch(self, run_id: str, mode: str, selected: list[dict],
                   bundle: dict | None, max_parallel: int):
        run_dir = release.STATE_ROOT / "reports" / run_id
        results = []
        try:
            run_dir.mkdir(parents=True, exist_ok=False)
            if mode != "erase-only":
                if not bundle:
                    raise ValueError("No prepared release")
                folder = next(iter(bundle["files"].values())).parent
                checked = release.load_local(folder)
                if checked["manifest"] != bundle["manifest"]:
                    raise ValueError("Prepared release changed after confirmation")
                # All files and network client requirements are available before touching hardware.
                if mode in ("network-install", "network-update"):
                    import socket
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as test_socket:
                        test_socket.bind(("0.0.0.0", 0))
            if mode != "network-update":
                connected = {p["serial"] for p in hardware.list_probes()}
                if any(item["probe_serial"] not in connected for item in selected):
                    raise ValueError("A selected ST-LINK was disconnected; no board was erased")
                if shutil.disk_usage(run_dir).free < len(selected) * 110 * 1024 * 1024:
                    raise ValueError("Insufficient disk space for full backups and readbacks of this batch")
            with ThreadPoolExecutor(max_workers=min(max_parallel, len(selected))) as executor:
                futures = [executor.submit(self._run_board, run_dir, mode, item, bundle)
                           for item in selected]
                for future in as_completed(futures):
                    results.append(future.result())
        except Exception as exc:
            with self.lock:
                self.general_error = str(exc)
                for board in self.boards.values():
                    if board["state"] == "queued":
                        board.update(state="failed", phase="not started", error=str(exc))
        finally:
            summary = {"run_id": run_id, "mode": mode,
                       "release_tag": bundle["release"]["tag"] if bundle else None,
                       "firmware_version": bundle["manifest"]["firmware_version"] if bundle else None,
                       "boards": results, "general_error": self.general_error,
                       "created_at": dt.datetime.now(dt.timezone.utc).isoformat()}
            summary_path = run_dir / "summary.json"
            try:
                summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
                with self.lock:
                    self.report_path = str(summary_path)
            except OSError as exc:
                with self.lock:
                    detail = f"Could not save the run summary: {exc}"
                    self.general_error = (self.general_error + "; " + detail).lstrip("; ")
            finally:
                with self.lock:
                    self.running = False
