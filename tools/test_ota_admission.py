"""Explicit on-board protocol-v2 admission test. May erase ONLY an inactive app slot.
Prefer tools/test_firmware_arm.py for tests that do not touch connected hardware.
"""
import argparse
import datetime as dt
import hmac
import json
from pathlib import Path
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"installer"))
import credentials
import ethernet
from image_format import NONE, pack
from operation_lock import board_lock

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uid",required=True)
    parser.add_argument("--ip",default="")
    parser.add_argument("--image",type=Path,required=True)
    parser.add_argument("--exercise-inactive-slot",action="store_true",required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args();uid=credentials.checked_uid(args.uid)
    record={"status":"failed","uid":uid,"checks":[],"transport":"ethernet","swd_used":False,
            "created_at":dt.datetime.now(dt.timezone.utc).isoformat()}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    try:
        with board_lock(uid):
            peer=ethernet.connect(uid,credentials.load_key(uid),args.ip)
            exercise(peer,args.image.read_bytes(),record)
            record["status"]="passed"
    except Exception as error:
        record["error"]=str(error)
        raise
    finally:
        args.output.write_text(json.dumps(record,indent=2),encoding="utf-8")
    print(f"{len(record['checks'])} admission checks passed; confirmed application unchanged.")

def exercise(peer,payload,record):
        uid=peer.uid
        before=peer.status();record["before"]=before
        image=pack(payload,before["generation_floor"]+1)
        if (before["role"]!="application" or not before["boot_confirmed"]
                or before["upload_state"] or before["trial_slot"]!=NONE):
            raise RuntimeError("A confirmed, idle application is required")
        checks=record["checks"]
        def rejects(action,body,reason):
            try: peer.call(action,body)
            except ethernet.AuthenticationError: raise
            except ethernet.UpdateError as error:
                if reason not in str(error): raise
                checks.append(reason);return
            raise AssertionError("Unexpected acceptance: "+action)
        # Replay a safe status request. Rejection must not consume a sequence.
        path=ethernet.PREFIX+"status";seq=struct.pack("<I",peer.sequence)
        mac=hmac.digest(peer.key,peer.nonce+seq+path.encode("ascii")+b"\0","sha256")
        peer.status()
        code,_,raw=peer._http("POST",path,seq+mac)
        assert code==401 and json.loads(raw)["error"]=="AUTHENTICATION_FAILED"
        checks.append("REPLAY_REJECTED");peer.status()
        wrong=ethernet.Client(peer.ip,uid,bytes(b^1 for b in peer.key))
        try: wrong.call("begin?uid="+uid,image[:128])
        except ethernet.AuthenticationError: checks.append("WRONG_KEY_REJECTED")
        else: raise AssertionError("Wrong key accepted")
        assert peer.status()["upload_state"]==0
        other=("F" if uid[0]!="F" else "E")+uid[1:]
        rejects("begin?uid="+other,image[:128],"UID_MISMATCH")
        bad=bytearray(image[:128]);bad[124]^=1
        rejects("begin?uid="+uid,bytes(bad),"IMAGE_HEADER_INVALID")
        rejects("begin?uid="+uid,pack(payload,before["generation_floor"])[:128],"GENERATION_NOT_NEWER")
        rejects("chunk?offset=0",image[4096:5120],"NO_ACTIVE_UPLOAD")
        begin=peer.call("begin?uid="+uid,image[:128])
        assert begin["target_slot"] not in (before["confirmed_slot"],before["running_slot"])
        checks.append("INACTIVE_SLOT_SELECTED")
        try:
            assert peer.call("chunk?offset=0",payload[:1024])["received"]==1024
            assert peer.call("chunk?offset=0",payload[:1024])["received"]==1024
            checks.append("DUPLICATE_CHUNK_IDEMPOTENT")
            altered=bytearray(payload[:1024]);altered[-1]^=1
            rejects("chunk?offset=0",bytes(altered),"OFFSET_MISMATCH")
            rejects("chunk?offset=2048",payload[2048:3072],"OFFSET_MISMATCH")
            rejects("chunk?offset=1024",b"x","CHUNK_INVALID")
            rejects("finish",b"","UPLOAD_INCOMPLETE");rejects("activate",b"","IMAGE_NOT_VERIFIED")
            # Readback succeeds, but the complete payload differs from its header.
            corrupt=bytearray(payload);corrupt[-1]^=1;size=begin["chunk_bytes"]
            for offset in range(1024,len(corrupt),size):
                peer.call(f"chunk?offset={offset}",bytes(corrupt[offset:offset+size]))
            rejects("finish",b"","PAYLOAD_DIGEST_MISMATCH")
            rejects("activate",b"","IMAGE_NOT_VERIFIED")
        finally: peer.call("abort")
        after=peer.status();record["after"]=after
        for field in ("confirmed_slot","confirmed_generation","metadata_sequence",
                      "running_sha256","running_slot","running_generation"):
            assert after[field]==before[field],field
        assert after["boot_confirmed"] and after["healthy"]
        assert after["trial_slot"]==NONE and after["upload_state"]==0
        checks.append("CONFIRMED_APPLICATION_UNCHANGED")

if __name__=="__main__": main()
