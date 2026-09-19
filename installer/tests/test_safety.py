"""Safety boundaries that must hold before any physical erase."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import controller
import hardware
import release


UID = "00112233445566778899AABB"
SERIAL = "0102030405060708090A0B0C"


class SafetyTests(unittest.TestCase):
    def test_uid_guard_requires_full_96_bits(self):
        self.assertIn("0x00112233 0x44556677 0x8899AABB", hardware.uid_guard(UID))
        for invalid in ("", "00112233", UID + "CC", "00112233445566778899AABZ"):
            with self.assertRaises(hardware.HardwareError):
                hardware.uid_guard(invalid)

    def test_missing_backup_aborts_before_erase(self):
        control = controller.Controller()
        control.boards[SERIAL] = {"uid": UID, "state": "queued"}
        with tempfile.TemporaryDirectory() as temporary, \
             mock.patch.object(hardware, "full_backup", side_effect=hardware.HardwareError("bad backup")), \
             mock.patch.object(hardware, "erase_all") as erase:
            result = control._run_board(Path(temporary), "erase-only",
                                        {"uid": UID, "probe_serial": SERIAL}, None)
            self.assertEqual(result["status"], "failed")
            self.assertIn("bad backup", result["error"])
            erase.assert_not_called()

    def test_confirmation_and_unique_uid_are_required(self):
        control = controller.Controller()
        control.inspected = {SERIAL: {"uid": UID, "probe_serial": SERIAL}}
        with self.assertRaisesRegex(ValueError, "Type ERASE"):
            control.start("erase-only", [SERIAL], "yes", 1)
        with self.assertRaisesRegex(ValueError, "distinct"):
            control.start("erase-only", [SERIAL, SERIAL], "ERASE 2", 2)
        self.assertFalse(control.running)

    def test_expected_layout_has_no_unintended_nor_data(self):
        import struct
        with tempfile.TemporaryDirectory() as temporary:
            app = bytearray(2048)
            struct.pack_into("<II", app, 0, 0x20020000, 0xC0000401)
            plan = hardware.prepare_install(Path(temporary), bytes(app), b"\xAB" * 5800)
            internal = (Path(temporary) / "expected-internal.bin").read_bytes()
            nor = (Path(temporary) / "expected-nor.bin").read_bytes()
            self.assertEqual(len(internal), hardware.INTERNAL_BYTES)
            self.assertEqual(len(nor), hardware.NOR_BYTES)
            self.assertEqual(internal[5800:], b"\xff" * (hardware.INTERNAL_BYTES - 5800))
            self.assertEqual(nor[0:0x10000], b"\xff" * 0x10000)
            for offset in hardware.IMAGE_OFFSETS:
                self.assertEqual(nor[offset:offset + len(plan["image"])], plan["image"])

    def test_tampered_release_binary_is_rejected(self):
        manifest = {"schema": 1, "release_tag": "v0.1.0",
                    "hardware": "MainBoard-v2.6-H750", "architecture": "nor-sdram-shadow-v1",
                    "files": {"g100-api.bin": {"bytes": 2048, "sha256": "0" * 64},
                              "stage0.bin": {"bytes": 5800, "sha256": "1" * 64}}}
        import json
        with tempfile.TemporaryDirectory() as temporary, \
             mock.patch.object(release, "STATE_ROOT", Path(temporary)), \
             mock.patch.object(release, "_fetch", side_effect=[json.dumps(manifest).encode(), b"X" * 2048]):
            with self.assertRaisesRegex(release.ReleaseError, "SHA-256"):
                release.fetch_bundle({"tag": "v0.1.0", "assets": {
                    "firmware-manifest.json": "https://example.com/manifest",
                    "g100-api.bin": "https://example.com/app",
                    "stage0.bin": "https://example.com/stage0"}})


if __name__ == "__main__":
    unittest.main()
