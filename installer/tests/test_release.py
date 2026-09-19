"""Release discovery and transport recovery without touching any board."""

import json
from pathlib import Path
import sys
import unittest
from unittest import mock
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release


class ReleaseTests(unittest.TestCase):
    def test_latest_uses_manifest_and_pins_binary_urls_to_its_tag(self):
        manifest = {"release_tag": "v0.1.1"}
        with mock.patch.object(release, "_fetch", return_value=json.dumps(manifest).encode()) as fetch:
            found = release.latest()
        fetch.assert_called_once_with(release.LATEST_MANIFEST, 64 * 1024)
        self.assertEqual(found["tag"], "v0.1.1")
        self.assertEqual(found["manifest"], manifest)
        self.assertEqual(found["assets"]["g100-api.bin"],
                         f"{release.RELEASES}/download/v0.1.1/g100-api.bin")

    def test_tls_failure_recovers_with_windows_curl(self):
        with mock.patch.object(release, "_urllib_fetch",
                               side_effect=urllib.error.URLError("TLS handshake timed out")), \
             mock.patch.object(release, "_windows_fetch", return_value=b"firmware") as fallback:
            self.assertEqual(release._fetch(release.LATEST_MANIFEST, 64 * 1024), b"firmware")
        fallback.assert_called_once_with(release.LATEST_MANIFEST, 64 * 1024,
                                         "Windows curl")

    def test_power_shell_is_used_when_curl_also_fails(self):
        with mock.patch.object(release, "_urllib_fetch",
                               side_effect=urllib.error.URLError("TLS handshake timed out")), \
             mock.patch.object(release, "_windows_fetch",
                               side_effect=[OSError("curl unavailable"), b"firmware"]) as fallback:
            self.assertEqual(release._fetch(release.LATEST_MANIFEST, 64 * 1024), b"firmware")
        self.assertEqual([call.args[2] for call in fallback.call_args_list],
                         ["Windows curl", "PowerShell"])

    def test_missing_asset_does_not_mask_http_404_with_fallback(self):
        error = urllib.error.HTTPError(release.LATEST_MANIFEST, 404, "Not found", {}, None)
        with mock.patch.object(release, "_urllib_fetch", side_effect=error), \
             mock.patch.object(release, "_windows_fetch") as fallback:
            with self.assertRaisesRegex(release.ReleaseError, "HTTP 404"):
                release._fetch(release.LATEST_MANIFEST)
        fallback.assert_not_called()


if __name__ == "__main__":
    unittest.main()
