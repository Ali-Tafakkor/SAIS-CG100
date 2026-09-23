"""Exercise an unconfirmed trial and automatic rollback on an enrolled board.

This intentionally restarts the board and takes about five minutes. It does not
claim to test a CPU crash. The candidate must be a compatible, healthy build
with a distinct digest. No SWD module or subprocess is used.
"""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "installer"))
import credentials
import ethernet
from image_format import NONE, pack
from operation_lock import board_lock


def exercise(peer, payload, version, record, save):
    before = peer.status()
    record["before"] = before
    if (before["role"] != "application" or not before["boot_confirmed"]
            or before["upload_state"] or before["trial_slot"] != NONE):
        raise RuntimeError("A confirmed, idle application is required")
    digest = hashlib.sha256(payload).hexdigest()
    if digest == before["running_sha256"]:
        raise ValueError("Use a candidate distinct from the confirmed application")
    generation = before["generation_floor"] + 1
    image = pack(payload, generation)
    record.update(candidate_sha256=digest, candidate_generation=generation)
    begin = peer.call("begin?uid=" + peer.uid, image[:128])
    assert begin["target_slot"] != before["confirmed_slot"]
    size = begin["chunk_bytes"]
    for offset in range(0, len(payload), size):
        reply = peer.call(f"chunk?offset={offset}", payload[offset:offset + size])
        assert reply["received"] == min(offset + size, len(payload))
    peer.call("finish")
    peer.call("activate", retry=False)
    print("Candidate activated; deliberately withholding confirmation.", flush=True)
    save()
    started = time.monotonic()
    seen_trial = False
    seen_nonces = set()
    last_event = None
    while time.monotonic() - started < 420:
        time.sleep(2)
        try:
            current = ethernet.Client(peer.ip, peer.uid, peer.key, timeout=3).status()
        except (OSError, ValueError, ethernet.UpdateError):
            continue
        event = (current["role"], current["running_generation"], current["trial_slot"],
                 current["nonce"])
        if event != last_event:
            record["events"].append({"seconds": round(time.monotonic()-started, 2), "state": current})
            save()
            print(f"{round(time.monotonic()-started)}s: {current['role']} "
                  f"generation {current['running_generation']}, trial {current['trial_slot']}", flush=True)
            last_event = event
        if current["role"] != "application":
            continue
        if current["running_generation"] == generation:
            assert current["running_sha256"] == digest and current["version"] == version
            assert not current["boot_confirmed"]
            seen_trial = True
            seen_nonces.add(current["nonce"])
        elif (seen_trial and current["trial_slot"] == NONE
              and current["running_sha256"] == before["running_sha256"]):
            assert current["confirmed_generation"] == before["confirmed_generation"]
            assert current["running_generation"] == before["running_generation"]
            assert current["boot_confirmed"] and current["healthy"]
            assert current["generation_floor"] >= generation
            assert len(seen_nonces) == 2, "Expected two distinct trial boots"
            record.update(after=current, trial_boots=len(seen_nonces),
                          elapsed_seconds=round(time.monotonic()-started, 2))
            return
    raise RuntimeError("The confirmed application did not return before the rollback deadline")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uid", required=True)
    parser.add_argument("--ip", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--exercise-trial-rollback", action="store_true", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    uid = credentials.checked_uid(args.uid)
    record = {"status": "failed", "test": "unconfirmed healthy trial timeout",
              "transport": "ethernet", "swd_used": False, "events": [],
              "created_at": dt.datetime.now(dt.timezone.utc).isoformat()}
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(record, indent=2), encoding="utf-8")

    try:
        with board_lock(uid):
            peer = ethernet.connect(uid, credentials.load_key(uid), args.ip)
            exercise(peer, args.image.read_bytes(), args.version, record, save)
            record["status"] = "passed"
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        save()
    print("Two unconfirmed trial boots rolled back to the original confirmed application.")


if __name__ == "__main__":
    main()
