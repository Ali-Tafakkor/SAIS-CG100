"""Cross-process board lock shared by UI and unattended updates."""
from contextlib import contextmanager
from credentials import checked_uid
from state_lock import file_lock

@contextmanager
def board_lock(uid):
    with file_lock(checked_uid(uid)):
        yield
