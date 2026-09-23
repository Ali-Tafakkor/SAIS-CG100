"""Small cross-process file lock for local registry and release-cache writers."""
from contextlib import contextmanager
import os
import re
import time
import release

@contextmanager
def file_lock(name: str, wait: float = 0):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        raise ValueError("Invalid lock name")
    folder=release.STATE_ROOT / "locks"
    folder.mkdir(parents=True,exist_ok=True)
    with (folder / (name+".lock")).open("a+b") as stream:
        if stream.tell()==0:
            stream.write(b"0");stream.flush()
        deadline=time.monotonic()+wait
        while True:
            stream.seek(0)
            try:
                if os.name=="nt":
                    import msvcrt
                    msvcrt.locking(stream.fileno(),msvcrt.LK_NBLCK,1)
                else:
                    import fcntl
                    fcntl.flock(stream.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
                break
            except OSError as exc:
                if time.monotonic()>=deadline:
                    raise RuntimeError("Another installer or fleet worker is using this board or state file") from exc
                time.sleep(.05)
        try:
            yield
        finally:
            stream.seek(0)
            if os.name=="nt": msvcrt.locking(stream.fileno(),msvcrt.LK_UNLCK,1)
            else: fcntl.flock(stream.fileno(),fcntl.LOCK_UN)
