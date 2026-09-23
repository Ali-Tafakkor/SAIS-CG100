"""Visible, diagnosable startup for the portable Windows installer."""
from __future__ import annotations

import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import subprocess
import threading
import webbrowser

import release

UI_READY = threading.Event()
LOG = logging.getLogger("g100.launcher")


def prepare() -> Path:
    """Fail before opening the UI if its state directory cannot be used."""
    release.STATE_ROOT.mkdir(parents=True, exist_ok=True)
    from state_lock import file_lock
    with file_lock("startup-check"):
        pass
    path = release.STATE_ROOT / "launcher.log"
    handler = RotatingFileHandler(path, maxBytes=1024*1024, backupCount=2, encoding="utf-8")
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
    LOG.addHandler(handler)
    LOG.setLevel(logging.INFO)
    LOG.info("Starting installer from %s", Path(__file__).resolve().parents[1])
    print(f"Startup log: {path}", flush=True)
    return path


def browser_candidates():
    """Use installed browser executables, without parsing shell command strings."""
    names = ("chrome.exe", "msedge.exe", "firefox.exe")
    found = []
    if os.name == "nt":
        import winreg
        for name in names:
            for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
                try:
                    with winreg.OpenKey(hive, "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"+name) as key:
                        value = winreg.QueryValueEx(key, None)[0]
                        found.append(Path(os.path.expandvars(value.strip('"'))))
                except OSError:
                    pass
        for folder in (os.environ.get("PROGRAMFILES"), os.environ.get("PROGRAMFILES(X86)"), os.environ.get("LOCALAPPDATA")):
            if folder:
                found.extend(Path(folder)/suffix for suffix in (
                    "Google/Chrome/Application/chrome.exe", "Microsoft/Edge/Application/msedge.exe",
                    "Mozilla Firefox/firefox.exe"))
    unique = []
    for candidate in found:
        if candidate.is_file() and candidate not in unique:
            unique.append(candidate)
    return unique


def open_browser(url: str):
    try:
        result = webbrowser.open(url, new=2)
        LOG.info("Default browser dispatch returned %s", result)
    except Exception as exc:
        LOG.warning("Default browser dispatch failed: %s", exc)
    if UI_READY.wait(5):
        LOG.info("Installer UI connected")
        return
    for executable in browser_candidates():
        try:
            subprocess.Popen([str(executable), url],
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            LOG.info("Browser fallback dispatched: %s", executable)
            if UI_READY.wait(5):
                LOG.info("Installer UI connected after fallback")
                return
        except OSError as exc:
            LOG.warning("Browser fallback failed: %s", exc)
    message = f"If the browser did not open, paste this address into your browser: {url}"
    LOG.warning("No UI connection detected. %s", message)
    print(message, flush=True)


def serve_notice(url: str, automatic: bool):
    UI_READY.clear()
    shortcut = release.STATE_ROOT / "Open-Installer.url"
    shortcut.write_text("[InternetShortcut]\nURL="+url+"\n", encoding="ascii")
    LOG.info("Listening at %s", url)
    print("SAIS-CG100 Programmer: " + url, flush=True)
    print("Keep this window open while using the installer.", flush=True)
    print(f"Manual browser shortcut: {shortcut}", flush=True)
    if automatic:
        threading.Thread(target=open_browser, args=(url,), daemon=True).start()
