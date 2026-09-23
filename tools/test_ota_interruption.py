"""Prepare an incomplete inactive image, then check recovery after a manual power cut.

Run 'prepare', disconnect/reconnect board power, then run 'check'. This proves
recovery from an incomplete transfer; it does not claim a cut at a precise NOR
programming or metadata-commit instruction. All board traffic is Ethernet.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "installer"))
import credentials
import ethernet
from image_format import MAX_IMAGE, NONE, pack
from operation_lock import board_lock


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("prepare", "check"))
    parser.add_argument("--uid", required=True)
    parser.add_argument("--ip", required=True)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exercise-inactive-slot", action="store_true")
    parser.add_argument("--power-cycle-confirmed", action="store_true")
    args = parser.parse_args()
    uid = credentials.checked_uid(args.uid)
    if args.stage == "prepare" and (not args.image or not args.exercise_inactive_slot):
        parser.error("prepare needs --image and --exercise-inactive-slot")
    if args.stage == "check" and not args.power_cycle_confirmed:
        parser.error("check requires an actual power cycle and --power-cycle-confirmed")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with board_lock(uid):
        key = credentials.load_key(uid)
        peer = ethernet.connect(uid, key, args.ip)
        if args.stage == "prepare":
            before = peer.status()
            if (before["role"] != "application" or not before["boot_confirmed"]
                    or before["upload_state"] or before["trial_slot"] != NONE):
                raise RuntimeError("A confirmed, idle application is required")
            payload = args.image.read_bytes()
            if len(payload) > MAX_IMAGE:
                raise ValueError("Source image exceeds capacity")
            payload += b"\xff" * (MAX_IMAGE - len(payload))
            image = pack(payload, before["generation_floor"] + 1)
            begin = peer.call("begin?uid=" + uid, image[:128])
            assert begin["target_slot"] != before["confirmed_slot"]
            size = begin["chunk_bytes"]
            for offset in range(0, 262144, size):
                peer.call(f"chunk?offset={offset}", payload[offset:offset + size])
            partial = peer.status()
            assert partial["upload_state"] == 1 and partial["received"] == 262144
            result = {"status": "awaiting physical power cycle", "uid": uid,
                      "transport": "ethernet", "swd_used": False, "before": before,
                      "partial": partial, "candidate_sha256": hashlib.sha256(payload).hexdigest()}
            args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
            print("262144 bytes written to the inactive slot; no finish or activation sent.")
            print("Now power-cycle the board, keep SWD disconnected, then run check.")
        else:
            result = json.loads(args.output.read_text(encoding="utf-8"))
            assert result["uid"] == uid
            before = result["before"]
            deadline = time.monotonic() + 75
            while True:
                current = peer.status()
                if current["role"] == "application":
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("Application did not leave recovery")
                time.sleep(1)
                peer = ethernet.connect(uid, key, args.ip, seconds=15)
            for field in ("running_sha256", "running_generation", "confirmed_slot", "confirmed_generation"):
                assert current[field] == before[field], field
            assert current["healthy"] and current["boot_confirmed"]
            assert current["nonce"] != before["nonce"]
            assert current["upload_state"] == 0 and current["trial_slot"] == NONE
            result.update(status="passed", after=current, physical_power_cycle="operator confirmed")
            args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
            print("Previous confirmed application recovered after interrupted transfer and power cycle.")


if __name__ == "__main__":
    main()
