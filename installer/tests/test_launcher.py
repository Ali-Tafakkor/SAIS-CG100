"""Regressions for a running server whose default browser never opens."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import launcher
import release


class LauncherTests(unittest.TestCase):
    def test_false_default_browser_result_uses_explicit_browser(self):
        with mock.patch.object(launcher.webbrowser, "open", return_value=False), \
             mock.patch.object(launcher, "browser_candidates", return_value=[Path("browser.exe")]), \
             mock.patch.object(launcher.UI_READY, "wait", side_effect=[False, True]), \
             mock.patch.object(launcher.subprocess, "Popen") as run:
            launcher.open_browser("http://127.0.0.1:8765/")
        self.assertEqual(run.call_args.args[0], ["browser.exe", "http://127.0.0.1:8765/"])

    def test_dispatch_success_without_ui_still_uses_fallback(self):
        with mock.patch.object(launcher.webbrowser, "open", return_value=True), \
             mock.patch.object(launcher, "browser_candidates", return_value=[Path("browser.exe")]), \
             mock.patch.object(launcher.UI_READY, "wait", side_effect=[False, True]), \
             mock.patch.object(launcher.subprocess, "Popen") as run:
            launcher.open_browser("http://127.0.0.1:8765/")
        run.assert_called_once()

    def test_connected_ui_does_not_open_another_browser(self):
        with mock.patch.object(launcher.webbrowser, "open", return_value=True), \
             mock.patch.object(launcher.UI_READY, "wait", return_value=True), \
             mock.patch.object(launcher.subprocess, "Popen") as run:
            launcher.open_browser("http://127.0.0.1:8765/")
        run.assert_not_called()

    def test_no_available_browser_prints_manual_address(self):
        with mock.patch.object(launcher.webbrowser, "open", side_effect=OSError("no association")), \
             mock.patch.object(launcher.UI_READY, "wait", return_value=False), \
             mock.patch.object(launcher, "browser_candidates", return_value=[]), \
             mock.patch("builtins.print") as output:
            launcher.open_browser("http://127.0.0.1:8765/")
        self.assertIn("http://127.0.0.1:8765/", output.call_args.args[0])

    def test_unwritable_state_stops_startup_before_opening_browser(self):
        with tempfile.TemporaryDirectory() as temporary:
            blocker=Path(temporary)/"blocked"
            blocker.write_text("file, not directory")
            with mock.patch.object(release,"STATE_ROOT",blocker), \
                 mock.patch.object(launcher.webbrowser,"open") as browser:
                with self.assertRaises(OSError): launcher.prepare()
            browser.assert_not_called()
