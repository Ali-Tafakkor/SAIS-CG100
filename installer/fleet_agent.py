"""Opt-in fleet updater. Run on a Windows host with LAN/VPN access to enrolled boards."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime as dt
import json
import time
import credentials
import ethernet
import release
from operation_lock import board_lock
from state_lock import file_lock

def update_one(entry: dict, bundle: dict) -> dict:
    uid = entry["uid"]
    transferring = False
    try:
        with board_lock(uid):
            current=next((item for item in credentials.fleet() if item["uid"]==uid),{})
            if current.get("auto_update") is not True:
                return {"uid":uid,"status":"skipped","reason":"Automatic updates disabled"}
            key = credentials.load_key(uid)
            peer = ethernet.connect(uid, key, entry.get("ip", ""), seconds=20)
            transferring = True
            result = ethernet.install(peer, bundle["files"]["g100-api.bin"].read_bytes(),
                                      bundle["manifest"]["firmware_version"])
            credentials.remember(uid, ip=result["ip"], last_release=bundle["release"]["tag"],
                last_version=bundle["manifest"]["firmware_version"], last_error="")
            return {"uid": uid, **result}
    except Exception as exc:
        credentials.remember(uid, last_error=str(exc))
        return {"uid": uid, "status": "failed" if transferring or isinstance(exc, ethernet.AuthenticationError) else "unreachable", "error": str(exc)}

def cycle(workers: int = 2) -> dict:
    with file_lock("fleet-agent"):
        return _cycle(workers)

def _cycle(workers: int = 2) -> dict:
    selected = [item for item in credentials.fleet() if item.get("auto_update") is True]
    if not selected:
        return {"status": "idle", "reason": "No boards opted in to automatic updates", "boards": []}
    bundle = release.fetch_bundle()
    if bundle["manifest"].get("update_protocol") != 2:
        raise ValueError("Latest release does not support network updates; no board was changed")
    # A failed release is held for operator review, avoiding an endless reboot loop.
    pending = [item for item in selected if item.get("last_release") != bundle["release"]["tag"]]
    folder = release.STATE_ROOT / "fleet-reports"
    folder.mkdir(parents=True, exist_ok=True)
    hold_path = folder / "holds.json"
    holds = json.loads(hold_path.read_text()) if hold_path.exists() else {}
    if holds.get("release") == bundle["release"]["tag"]:
        return {"status": "held", "reason": "A candidate failed; review results and explicitly resume this release", "boards": []}
    pending = [item for item in pending if holds.get(item["uid"]) != bundle["release"]["tag"]]
    results = []
    # Canary first. Do not fan out a release that failed on the first selected board.
    for index, entry in enumerate(pending):
        first = update_one(entry, bundle)
        results.append(first)
        if first["status"] == "unreachable":
            continue  # Offline boards are retried on the next scheduled cycle.
        if first["status"] == "passed":
            with ThreadPoolExecutor(max_workers=workers) as pool:
                results.extend(pool.map(lambda item: update_one(item, bundle), pending[index+1:]))
        break
    for result in results:
        if result["status"] == "failed":
            holds[result["uid"]] = bundle["release"]["tag"]
            holds["release"] = bundle["release"]["tag"]
    hold_path.write_text(json.dumps(holds, indent=2), encoding="utf-8")
    report = {"at": dt.datetime.now(dt.timezone.utc).isoformat(), "release": bundle["release"]["tag"],
              "boards": results, "held": holds, "status": "complete"}
    (folder / "latest.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report

def clear_holds():
    with file_lock("fleet-agent"):
        path = release.STATE_ROOT / "fleet-reports/holds.json"
        if path.exists():
            path.write_text("{}", encoding="utf-8")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--watch", action="store_true", help="Keep checking while this process runs")
    parser.add_argument("--interval", type=int, default=3600)
    parser.add_argument("--workers", type=int, choices=range(1, 9), default=2)
    args = parser.parse_args()
    if args.interval < 60:
        parser.error("Polling interval must be at least 60 seconds")
    while True:
        try:
            print(json.dumps(cycle(args.workers), indent=2), flush=True)
        except Exception as exc:
            print(json.dumps({"status": "failed", "error": str(exc)}), flush=True)
            if not args.watch:
                raise SystemExit(1)
        if not args.watch:
            return
        time.sleep(args.interval)

if __name__ == "__main__":
    main()
