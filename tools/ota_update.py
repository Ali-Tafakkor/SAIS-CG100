"""Authenticated Ethernet-only installation using an enrolled board's Windows credentials."""
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"installer"))
import credentials
import ethernet
from operation_lock import board_lock

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--uid",required=True)
    p.add_argument("--ip",default="")
    p.add_argument("--image",type=Path,required=True)
    p.add_argument("--version",required=True)
    p.add_argument("--output",type=Path,required=True)
    a=p.parse_args()
    uid=credentials.checked_uid(a.uid)
    with board_lock(uid):
        peer=ethernet.connect(uid,credentials.load_key(uid),a.ip)
        result=ethernet.install(peer,a.image.read_bytes(),a.version)
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(json.dumps(result,indent=2),encoding="utf-8")
        print(json.dumps(result,indent=2))

if __name__=="__main__": main()
