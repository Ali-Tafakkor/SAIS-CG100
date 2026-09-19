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

PHASE_SECONDS = {"backup": 540, "erase": 240, "blank check": 300,
                 "program": 180, "readback": 300, "Ethernet tests": 45}


class Controller:
    def __init__(self):
        self.lock = threading.RLock()
        self.probes: list[dict] = []
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
                        "url": self.bundle["release"]["url"]},
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
        if mode not in ("install", "erase-only"):
            raise ValueError("Choose install or erase-only")
        if not serials or len(serials) != len(set(serials)):
            raise ValueError("Select distinct inspected boards")
        if confirmation != f"ERASE {len(serials)}":
            raise ValueError(f"Type ERASE {len(serials)} to confirm deletion")
        if not 1 <= max_parallel <= 8:
            raise ValueError("Parallel worker count must be 1–8")
        with self.lock:
            if self.running:
                raise ValueError("A run is already in progress")
            if not set(serials) <= self.inspected.keys():
                raise ValueError("Inspect every selected board first")
            selected = [self.inspected[s].copy() for s in serials]
            if any("uid" not in item or "error" in item for item in selected):
                raise ValueError("Every selected board must pass inspection")
            if len({item["uid"] for item in selected}) != len(selected):
                raise ValueError("Selected MCU UIDs must be unique")
            if mode == "install" and (not self.bundle or
                                       self.bundle["release"]["tag"] != release_tag):
                raise ValueError("Fetch and review the current firmware release first")
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
        serial, uid = item["probe_serial"], item["uid"]
        directory = run_dir / f"board-{uid}"
        directory.mkdir(parents=True, exist_ok=False)
        begun = time.monotonic()
        record = {"uid": uid, "probe_serial": serial, "mode": mode,
                  "pre_erase_inspection": item, "checks": [], "status": "failed"}
        self._update(serial, started_at=dt.datetime.now(dt.timezone.utc).isoformat())
        try:
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
            plan = hardware.prepare_install(directory, app, stage0.read_bytes())
            hardware.program(serial, uid, directory, plan, stage0)
            record["checks"].append("Factory, A and B images plus boot metadata verified before Stage 0")
            self._phase(serial, "readback", 80, begun)
            record["final_readback"] = hardware.verify_post_write(serial, uid, directory, plan)
            record["checks"].append("Full internal/NOR readback equals the expected firmware layout")
            self._phase(serial, "Ethernet tests", 94, begun)
            record["network"] = network.test_runtime(
                uid, bundle["manifest"]["firmware_version"], len(app))
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
            (directory / "result.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        return record

    def _run_batch(self, run_id: str, mode: str, selected: list[dict],
                   bundle: dict | None, max_parallel: int):
        run_dir = release.STATE_ROOT / "reports" / run_id
        run_dir.mkdir(parents=True, exist_ok=False)
        results = []
        try:
            if mode == "install":
                current = release.latest()
                if not bundle or current["tag"] != bundle["release"]["tag"]:
                    raise ValueError("The latest release changed after confirmation; rescan and confirm again")
                bundle = release.fetch_bundle(current)
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
            summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
            with self.lock:
                self.report_path = str(summary_path)
                self.running = False
