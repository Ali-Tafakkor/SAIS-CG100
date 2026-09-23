"""Installer transport boundaries, blank-board layout and credential persistence."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import controller
import credentials
import ethernet
import fleet_agent
import hardware
import release
from image_format import meta_unpack, NONE
from operation_lock import board_lock

UID = "00112233445566778899AABB"
SERIAL = "0102030405060708090A0B0C"

def app_bytes(size=2048):
    data = bytearray(size)
    struct.pack_into("<II", data, 0, 0x20020000, 0xC0000401)
    return bytes(data)

class NetworkInstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.patch = mock.patch.object(release, "STATE_ROOT", self.folder / "state")
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.addCleanup(self.temp.cleanup)

    def bundle(self):
        files = {}
        for name, data in (("stage0.bin", b"X"*6008), ("recovery.bin", app_bytes()), ("g100-api.bin", app_bytes(32768))):
            path = self.folder / name
            path.write_bytes(data)
            files[name] = path
        manifest = {"schema": 2, "update_protocol": 2, "release_tag": "v0.2.0-test",
            "hardware": "MainBoard-v2.6-H750", "architecture": "nor-sdram-shadow-v1",
            "firmware_version": "test", "files": {name: {"bytes": p.stat().st_size,
            "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for name, p in files.items()}}
        (self.folder / "firmware-manifest.json").write_text(json.dumps(manifest))
        return {"files": files, "manifest": manifest, "release": {"tag": "v0.2.0-test", "url": ""}}

    def test_bootstrap_contains_no_main_application(self):
        record = credentials.provision_record(UID, bytes(range(32)))
        plan = hardware.prepare_install(self.folder, app_bytes(32768), b"X"*6008,
            recovery=app_bytes(), provision=record, bootstrap_only=True)
        nor = (self.folder / "expected-nor.bin").read_bytes()
        self.assertEqual(set(plan["images"]), {2})
        self.assertEqual(nor[0x100000:0x900000], b"\xff"*0x800000)
        self.assertEqual(nor[0xE0000:0xE0040], record)
        self.assertEqual(meta_unpack(nor[0xF0000:0xF0040])["confirmed_slot"], NONE)

    def test_large_application_is_independent_of_small_recovery_capacity(self):
        plan = hardware.prepare_install(self.folder, app_bytes(1024*1024), b"X"*6008,
            recovery=app_bytes(), provision=credentials.provision_record(UID, bytes(range(32))))
        self.assertGreater(len(plan["images"][0]), 0xD0000)
        self.assertLess(len(plan["images"][2]), 0xD0000)

    def test_no_swd_calls_after_bootstrap_program_exits(self):
        control = controller.Controller();control.boards[SERIAL] = {}
        events = []
        closed = False
        def swd(name):
            def call(*args, **kwargs):
                nonlocal closed
                self.assertFalse(closed, f"SWD reopened for {name}")
                events.append(name)
                if name == "program": closed = True
                return {}
            return call
        def transfer(*args, **kwargs):
            self.assertTrue(closed)
            events.append("ethernet")
            return {"status": "passed", "ip": "192.0.2.1"}
        with mock.patch.object(hardware, "full_backup", side_effect=swd("backup")), \
             mock.patch.object(hardware, "erase_all", side_effect=swd("erase")), \
             mock.patch.object(hardware, "verify_post_write", side_effect=swd("blank")), \
             mock.patch.object(hardware, "program", side_effect=swd("program")), \
             mock.patch.object(control, "_network_install", side_effect=transfer), \
             mock.patch.object(credentials, "ensure_key", return_value=bytes(range(32))):
            result = control._run_board(self.folder, "network-install",
                {"uid": UID, "probe_serial": SERIAL}, self.bundle())
        self.assertEqual(result["status"], "passed", result)
        self.assertEqual(events, ["backup", "erase", "blank", "program", "ethernet"])

    def test_network_only_run_cannot_enumerate_or_access_stlink(self):
        control=controller.Controller();control.boards[UID]={}
        with mock.patch.object(hardware, "list_probes", side_effect=AssertionError("SWD forbidden")), \
             mock.patch.object(hardware, "run_openocd", side_effect=AssertionError("SWD forbidden")), \
             mock.patch.object(control, "_network_install", return_value={"status":"passed"}):
            control._run_batch("network-test", "network-update", [{"uid":UID,"probe_serial":UID}],self.bundle(),1)
        self.assertEqual(control.boards[UID]["state"], "passed", control.state())

    def test_missing_recovery_fails_before_erase(self):
        control=controller.Controller()
        control.inspected[SERIAL]={"uid":UID,"probe_serial":SERIAL}
        bundle=self.bundle();bundle["manifest"]["update_protocol"]=1;control.bundle=bundle
        with mock.patch.object(hardware,"erase_all") as erase:
            with self.assertRaisesRegex(ValueError,"lacks the Ethernet bootstrap"):
                control.start("network-install",[SERIAL],"ERASE 1",1,bundle["release"]["tag"])
        erase.assert_not_called()

    def test_report_directory_failure_stops_before_hardware_and_releases_run(self):
        control = controller.Controller()
        control.running = True
        control.boards[SERIAL] = {"state": "queued"}
        release.STATE_ROOT.mkdir()
        (release.STATE_ROOT / "reports").write_text("a file blocks the report directory")
        with mock.patch.object(hardware, "list_probes") as probes:
            control._run_batch("unwritable", "erase-only",
                [{"uid": UID, "probe_serial": SERIAL}], None, 1)
        probes.assert_not_called()
        self.assertFalse(control.running)
        self.assertEqual(control.boards[SERIAL]["state"], "failed")
        self.assertIn("Could not save the run summary", control.general_error)
        self.assertIsNone(control.report_path)

    def test_dpapi_key_survives_restart_and_is_not_plaintext(self):
        if sys.platform != "win32": self.skipTest("Windows DPAPI")
        key=credentials.ensure_key(UID)
        saved=(release.STATE_ROOT / "credentials" / (UID+".dpapi")).read_bytes()
        self.assertNotIn(key,saved)
        self.assertEqual(credentials.load_key(UID),key)
        self.assertEqual(credentials.ensure_key(UID),key)
        credentials.remember(UID,auto_update=True)
        self.assertNotIn(key.hex(),json.dumps(controller.Controller().state()))

    def test_offline_release_tamper_is_rejected(self):
        self.bundle()
        self.assertEqual(release.load_local(self.folder)["manifest"]["update_protocol"],2)
        (self.folder / "recovery.bin").write_bytes(b"Q"*2048)
        with self.assertRaisesRegex(release.ReleaseError,"SHA-256"):
            release.load_local(self.folder)

    def test_board_lock_excludes_second_process_handle(self):
        with board_lock(UID):
            with self.assertRaisesRegex(RuntimeError,"Another installer"):
                with board_lock(UID): pass

    def test_fleet_does_not_download_or_update_without_opt_in(self):
        credentials.remember(UID)
        with mock.patch.object(release,"fetch_bundle") as fetch:
            self.assertEqual(fleet_agent.cycle()["status"],"idle")
        fetch.assert_not_called()

    def test_failed_canary_holds_entire_release_across_cycles(self):
        other="10112233445566778899AABB"
        credentials.remember(UID,auto_update=True);credentials.remember(other,auto_update=True)
        with mock.patch.object(release,"fetch_bundle",return_value=self.bundle()), \
             mock.patch.object(fleet_agent,"update_one",return_value={"uid":UID,"status":"failed"}) as update:
            first=fleet_agent.cycle();second=fleet_agent.cycle()
        self.assertEqual(update.call_count,1)
        self.assertEqual(first["held"]["release"],"v0.2.0-test")
        self.assertEqual(second["status"],"held")

    def test_offline_board_does_not_receive_a_permanent_release_hold(self):
        credentials.remember(UID,auto_update=True)
        with mock.patch.object(release,"fetch_bundle",return_value=self.bundle()), \
             mock.patch.object(fleet_agent,"update_one",return_value={"uid":UID,"status":"unreachable"}) as update:
            first=fleet_agent.cycle();second=fleet_agent.cycle()
        self.assertEqual(update.call_count,2)
        self.assertEqual(first["held"],{})

    def test_tampered_authenticated_response_is_never_success(self):
        client=ethernet.Client("192.0.2.1",UID,bytes(32))
        client.nonce=bytes(32);client.sequence=1
        client._http=mock.Mock(return_value=(200,{"X-G100-MAC":"0"*64},b'{"ok":true}'))
        with self.assertRaises(ethernet.AuthenticationError): client.status()

    def test_known_routed_address_is_retried_after_a_reboot_timeout(self):
        peer = mock.Mock()
        peer.status.side_effect = [OSError("rebooting"), {"ok": True}]
        with mock.patch.object(ethernet, "Client", return_value=peer) as factory, \
             mock.patch.object(ethernet, "discover", return_value=[]) as discovery, \
             mock.patch.object(ethernet.time, "sleep"), \
             mock.patch.object(ethernet.time, "monotonic", side_effect=[0, 0, .1, .2, .3, 99]):
            self.assertIs(ethernet.connect(UID, bytes(32), "192.0.2.1", seconds=5), peer)
        self.assertEqual(factory.call_count, 2)
        self.assertEqual(discovery.call_count, 1)

    def test_swd_runtime_acceptance_waits_for_app_and_uses_authenticated_api(self):
        data=app_bytes();digest=hashlib.sha256(data).hexdigest()
        peer=mock.Mock(ip="192.0.2.7")
        peer.status.side_effect=[{"role":"recovery"},{"role":"application","running_sha256":digest,
            "version":"test","healthy":True,"boot_confirmed":True}]
        with mock.patch.object(ethernet,"connect",return_value=peer), mock.patch.object(ethernet.time,"sleep"):
            result=ethernet.verify_installed(UID,bytes(32),data,"test")
        self.assertEqual(result["status"],"passed")
        self.assertTrue(result["authenticated"])
        peer.call.assert_not_called()

if __name__ == "__main__": unittest.main()
