"""An access-file import must not replace keys during another board operation."""
import base64
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import credentials
import main as server_main
import release
from operation_lock import board_lock

UID = "00112233445566778899AABB"


class KeyImportTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        patch = mock.patch.object(release, "STATE_ROOT", Path(folder.name))
        patch.start()
        self.addCleanup(patch.stop)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), server_main.Handler)
        worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        worker.start()

        def close():
            self.server.shutdown()
            self.server.server_close()
            worker.join(timeout=2)

        self.addCleanup(close)

    def import_key(self):
        record = credentials.provision_record(UID, bytes(range(32)))
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.server.server_port}/api/fleet/import",
            data=json.dumps({"record": base64.b64encode(record).decode()}).encode(),
            headers={"Content-Type": "application/json", "X-G100-Token": server_main.TOKEN})
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            response = opener.open(request, timeout=3)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def test_import_fails_without_touching_key_while_board_locked(self):
        with board_lock(UID), mock.patch.object(credentials, "store_key") as store:
            code, reply = self.import_key()
        self.assertEqual(code, 400)
        self.assertIn("Another installer", reply["error"])
        store.assert_not_called()

    def test_idle_board_can_import_and_register_its_key(self):
        with mock.patch.object(credentials, "store_key") as store:
            code, reply = self.import_key()
        self.assertEqual(code, 200, reply)
        store.assert_called_once_with(UID, bytes(range(32)))
        self.assertEqual(reply["board"]["uid"], UID)


if __name__ == "__main__":
    unittest.main()
