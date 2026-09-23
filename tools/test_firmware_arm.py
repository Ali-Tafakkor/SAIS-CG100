"""Compile and execute actual update/auth/boot C code in Unicorn, with NOR fault injection.

Test dependency: pip install --target .cache/test-deps unicorn==2.1.4
Uses the same pinned ARM GCC and Mbed TLS sources as the firmware build.
"""
from __future__ import annotations
import hashlib
import hmac
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / ".cache/test-deps"), str(ROOT / "installer")]
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR
import credentials
import ethernet
from image_format import pack, meta_pack, meta_unpack, NONE

UID = "00112233445566778899AABB"
KEY = bytes(range(32))  # Public test fixture; never used to provision hardware.
OUT = ROOT / ".local-test/arm"

def build():
    compiler = Path(os.environ.get("G100_COMPILER_BIN", ROOT / "toolchain/xpack-arm-none-eabi-gcc-15.2.1-1.1/bin"))
    if not compiler.exists():
        compiler = ROOT.parent / "toolchain/xpack-arm-none-eabi-gcc-15.2.1-1.1/bin"
    gcc = compiler / "arm-none-eabi-gcc.exe"
    app = ROOT / "SetAPIs_Calling/firmware/app"
    platform = ROOT / "firmware/memory-platform"
    vendor = ROOT / "firmware/ethernet-api/vendor"
    crypto = ROOT / "toolchain/crypto/mbedtls-3.6.5"
    includes = [app, platform, vendor / "Core/Inc", vendor / "Drivers/CMSIS/Include",
                vendor / "Drivers/CMSIS/Device/ST/STM32H7xx/Include",
                vendor / "Drivers/STM32H7xx_HAL_Driver/Inc", crypto / "include", crypto / "library"]
    sources = [app / "ota_update.c", app / "ota_auth.c", platform / "boot_state.c", platform / "boot_policy.c", platform / "config_store.c",
               ROOT / "installer/tests/arm_harness.c"]
    sources += [crypto / ("library/" + name + ".c") for name in ("sha256", "md", "platform", "platform_util", "constant_time")]
    OUT.mkdir(parents=True, exist_ok=True)
    flags = ["-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=soft", "-Os", "-DSTM32H750xx", "-DUSE_HAL_DRIVER",
             "-DG100_SHADOW", "-DG100_RECOVERY", "-DMBEDTLS_CONFIG_FILE=<rj_crypto_config.h>",
             "-ffunction-sections", "-fdata-sections"] + ["-I" + str(p) for p in includes]
    objects = []
    for source in sources:
        obj = OUT / (source.stem + ".o")
        subprocess.run([str(gcc), *flags, "-c", str(source), "-o", str(obj)], check=True, capture_output=True)
        objects.append(str(obj))
    elf = OUT / "harness.elf"
    keep = ["test_http", "test_discovery", "test_init", "test_fault", "test_health", "test_rng_fail", "test_boot", "test_valid", "test_config_save", "test_config_load"]
    subprocess.run([str(gcc), *flags, *objects, "-T" + str(ROOT / "installer/tests/arm_harness.ld"),
        "-specs=nano.specs", "-specs=nosys.specs", "-nostartfiles", "-Wl,--gc-sections",
        *["-Wl,--undefined=" + symbol for symbol in keep], "-o", str(elf)], check=True, capture_output=True)
    subprocess.run([str(compiler / "arm-none-eabi-objcopy.exe"), "-O", "binary", str(elf), str(OUT / "harness.bin")], check=True)
    symbols = subprocess.check_output([str(compiler / "arm-none-eabi-nm.exe"), str(elf)], text=True)
    return {line.split()[2]: int(line.split()[0], 16) for line in symbols.splitlines() if len(line.split()) == 3}

class Board:
    symbols = {}
    def __init__(self, nor=None, slot=2, generation=1, flags=0, seed=1):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for address, size in ((0x08000000, 0x200000), (0x20000000, 0x20000), (0x24000000, 0x10000),
                              (0x90000000, 0x1000000)):
            self.cpu.mem_map(address, size)
        self.cpu.mem_write(0x08000000, (OUT / "harness.bin").read_bytes())
        self.cpu.mem_write(0x90000000, nor or b"\xff" * 0x1000000)
        if nor is None:
            self.cpu.mem_write(0x900E0000, credentials.provision_record(UID, KEY))
            self.cpu.mem_write(0x900F0000, meta_pack(1, NONE, 0))
        self.call("test_init", slot, generation, flags, seed)

    def call(self, name, *args):
        for reg, arg in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.cpu.reg_write(reg, arg & 0xFFFFFFFF)
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001FFF0)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x081F0001)
        self.cpu.emu_start(self.symbols[name] | 1, 0x081F0000, count=100000000)
        return self.cpu.reg_read(UC_ARM_REG_R0)

    def http(self, method, path, data=None):
        data = data or b""
        io = struct.pack("<I", len(data)) + method.encode().ljust(8, b"\0") + path.encode().ljust(160, b"\0")
        self.cpu.mem_write(0x24001000, io + data.ljust(16420, b"\0") + bytes(1601))
        code = self.call("test_http")
        result = bytes(self.cpu.mem_read(0x24001000 + 172 + 16420, 1536)).split(b"\0", 1)[0]
        mac = bytes(self.cpu.mem_read(0x24001000 + 172 + 16420 + 1536, 65)).split(b"\0", 1)[0].decode()
        return code, {"X-G100-MAC": mac}, result

    def client(self, key=KEY):
        client = ethernet.Client("127.0.0.1", UID, key)
        client._http = self.http
        return client

    def nor(self):
        return bytes(self.cpu.mem_read(0x90000000, 0x1000000))

    def metadata(self):
        items = [meta_unpack(bytes(self.cpu.mem_read(address, 64))) for address in (0x900F0000, 0x900F1000)]
        return max((m for m in items if m), key=lambda m: m["sequence"])

def payload(size=32768, marker=7):
    data = bytearray([marker] * size)
    struct.pack_into("<II", data, 0, 0x20020000, 0xC0000401)
    return bytes(data)

def upload(board, data, generation=1):
    client = board.client()
    image = pack(data, generation)
    client.status()
    client.call("begin?uid=" + UID, image[:128])
    for offset in range(0, len(data), 16384):
        client.call(f"chunk?offset={offset}", data[offset:offset+16384])
    client.call("finish")
    return client

class FirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        Board.symbols = build()

    def test_update_discovery_is_padded_bounded_and_contains_no_key(self):
        board=Board();query=b"G100_UPDATE_DISCOVER_V2".ljust(256,b"\0")
        board.cpu.mem_write(0x24001000,struct.pack("<I",len(query)))
        board.cpu.mem_write(0x24001000+172,query)
        count=board.call("test_discovery")
        self.assertGreater(count,0);self.assertLessEqual(count,len(query))
        raw=bytes(board.cpu.mem_read(0x24001000+172+16420,count))
        self.assertEqual(json.loads(raw),{"protocol":2,"uid":UID,"firmware":"1.0.0-recovery"})
        self.assertNotIn(KEY,raw)
        board.cpu.mem_write(0x24001000+172+255,b"!")
        self.assertEqual(board.call("test_discovery"),0)
        board.cpu.mem_write(0x24001000,struct.pack("<I",22))
        self.assertEqual(board.call("test_discovery"),0)

    def test_first_install_confirm_and_second_update(self):
        board = Board()
        data = payload()
        client = upload(board, data)
        self.assertEqual(board.metadata()["confirmed_slot"], NONE)
        client.call("activate", retry=False)
        self.assertEqual(board.call("test_boot"), 0)
        running = Board(board.nor(), 0, 1, 1, seed=2)
        current = running.client()
        self.assertFalse(current.status()["boot_confirmed"])
        current.call("confirm", hashlib.sha256(data).digest())
        self.assertEqual(running.metadata()["confirmed_slot"], 0)
        next_client = upload(running, payload(marker=8), 2)
        next_client.call("activate", retry=False)
        self.assertEqual(running.call("test_boot"), 1)

    def test_wrong_key_replay_and_wrong_uid_never_erase(self):
        board = Board(); before = board.nor()
        with self.assertRaises(ethernet.AuthenticationError):
            board.client(bytes(32)).status()
        self.assertEqual(before, board.nor())
        c = board.client(); c.challenge()
        seq = struct.pack("<I", c.sequence); path = ethernet.PREFIX + "status"
        raw = seq + hmac.digest(KEY, c.nonce + seq + path.encode() + b"\0", "sha256")
        self.assertEqual(board.http("POST", path, raw)[0], 200)
        self.assertEqual(board.http("POST", path, raw)[0], 401)
        c.nonce = b""
        with self.assertRaisesRegex(ethernet.UpdateError, "UID_MISMATCH"):
            c.call("begin?uid=" + "F"*24, pack(payload(), 1)[:128])
        self.assertEqual(before, board.nor())

    def test_corrupt_payload_and_incomplete_upload_never_commit(self):
        board = Board(); c = board.client(); c.status()
        c.call("begin?uid=" + UID, pack(payload(), 1)[:128])
        c.call("chunk?offset=0", payload(marker=8)[:16384])
        with self.assertRaisesRegex(ethernet.UpdateError, "UPLOAD_INCOMPLETE"):
            c.call("finish")
        c.call("chunk?offset=16384", payload()[16384:])
        with self.assertRaisesRegex(ethernet.UpdateError, "PAYLOAD_DIGEST_MISMATCH"):
            c.call("finish")
        self.assertEqual(board.call("test_valid", 0), 0)
        self.assertEqual(board.metadata()["trial_slot"], NONE)

    def test_lost_chunk_response_can_retry_without_erase(self):
        board = Board();c = board.client();c.status()
        c.call("begin?uid=" + UID, pack(payload(), 1)[:128])
        first = c.call("chunk?offset=0", payload()[:16384])
        before = board.nor()
        self.assertEqual(c.call("chunk?offset=0", payload()[:16384]), first)
        self.assertEqual(before, board.nor())

    def test_failed_trial_rolls_back_and_next_upload_is_allowed(self):
        board = Board();data = payload();c = upload(board, data);c.call("activate", retry=False)
        board.call("test_boot")
        running = Board(board.nor(), 0, 1, 1, 2);c=running.client();c.status();c.call("confirm", hashlib.sha256(data).digest())
        c=upload(running, payload(marker=8), 2);c.call("activate", retry=False)
        self.assertEqual(running.call("test_boot"), 1)
        self.assertEqual(running.call("test_boot"), 1)
        self.assertEqual(running.call("test_boot"), 0)
        self.assertEqual(running.metadata()["trial_slot"], NONE)
        recovered = Board(running.nor(), 0, 1, 0, 3)
        upload(recovered, payload(marker=9), 3)

    def test_power_loss_during_metadata_commit_keeps_previous_record(self):
        for boundary in range(3):
            with self.subTest(boundary=boundary):
                board = Board();c=upload(board, payload());old=board.metadata()
                board.call("test_fault", boundary)
                with self.assertRaises(ethernet.UpdateError):
                    c.call("activate", retry=False)
                rebooted = Board(board.nor(), seed=2)
                self.assertEqual(rebooted.metadata(), old)

    def test_power_loss_during_payload_keeps_factory_and_no_valid_candidate(self):
        board=Board();before=board.nor()[:0x100000];c=board.client();c.status()
        c.call("begin?uid="+UID, pack(payload(),1)[:128]);board.call("test_fault",4)
        with self.assertRaises(ethernet.UpdateError):
            c.call("chunk?offset=0",payload()[:16384])
        rebooted=Board(board.nor(),seed=2)
        self.assertEqual(rebooted.nor()[:0x100000],before)
        self.assertEqual(rebooted.call("test_valid",0),0)
        upload(rebooted,payload(),1)

    def test_rng_failure_and_unhealthy_candidate_fail_closed(self):
        board=Board();board.call("test_rng_fail")
        with self.assertRaises(ethernet.AuthenticationError): board.client().status()
        board=Board();c=upload(board,payload());c.call("activate",retry=False);board.call("test_boot")
        running=Board(board.nor(),0,1,1,2);running.call("test_health",0);c=running.client();c.status()
        with self.assertRaisesRegex(ethernet.UpdateError,"RUNNING_IMAGE_NOT_READY"):
            c.call("confirm",hashlib.sha256(payload()).digest())

    def test_configuration_survives_reboot_and_torn_save(self):
        board=Board(); address=0x24001000+172
        original=b'{"network":{"mode":"static"}}'
        board.cpu.mem_write(address,original+b"\0")
        board.cpu.mem_write(address+2048,b"[]\0")
        self.assertEqual(board.call("test_config_save"),1)
        nor=board.nor()
        for boundary in range(4):
            rebooted=Board(nor,seed=2)
            self.assertEqual(rebooted.call("test_config_load"),1)
            self.assertEqual(bytes(rebooted.cpu.mem_read(address,len(original))),original)
            rebooted.cpu.mem_write(address,b'{"network":{"mode":"dhcp"}}\0')
            rebooted.call("test_fault",boundary)
            self.assertEqual(rebooted.call("test_config_save"),0)
            after=Board(rebooted.nor(),seed=3)
            self.assertEqual(after.call("test_config_load"),1)
            self.assertEqual(bytes(after.cpu.mem_read(address,len(original))),original)
            self.assertEqual(after.nor()[0xE0000:0xE0040],nor[0xE0000:0xE0040])

    def test_transport_retry_after_lost_authenticated_chunk_response(self):
        board=Board();c=board.client();c.status()
        c.call("begin?uid="+UID,pack(payload(),1)[:128])
        original=board.http; dropped=False
        def flaky(method,path,data=None):
            nonlocal dropped
            response=original(method,path,data)
            if "chunk?" in path and not dropped:
                dropped=True
                raise OSError("Connection lost after NOR write")
            return response
        c._http=flaky
        self.assertEqual(c.call("chunk?offset=0",payload()[:16384])["received"],16384)

    def test_host_install_against_real_arm_api_over_local_tcp(self):
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        import threading
        state={"board":Board(),"booted":False}
        class Handler(BaseHTTPRequestHandler):
            def handle_request(self):
                data=self.rfile.read(int(self.headers.get("Content-Length","0")))
                code,headers,raw=state["board"].http(self.command,self.path,data)
                self.send_response(code)
                for k,v in headers.items(): self.send_header(k,v)
                self.send_header("Content-Length",str(len(raw)));self.end_headers();self.wfile.write(raw)
            do_GET=handle_request
            do_POST=handle_request
            def log_message(self,*args): pass
        with ThreadingHTTPServer(("127.0.0.1",0),Handler) as server:
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                peer=ethernet.Client("127.0.0.1",UID,KEY,port=server.server_port)
                def reconnect(uid,key,ip,seconds):
                    if not state["booted"]:
                        selected=state["board"].call("test_boot")
                        state["board"]=Board(state["board"].nor(),selected,1,1,seed=2)
                        state["booted"]=True
                    return ethernet.Client(ip,uid,key,port=server.server_port)
                result=ethernet.install(peer,payload(),"1.0.0-recovery",reconnect=reconnect)
                self.assertEqual(result["status"],"passed")
                self.assertTrue(result["runtime"]["boot_confirmed"])
                self.assertFalse(result["swd_used"])
            finally:
                server.shutdown();thread.join()

    def test_both_metadata_sectors_lost_can_install_from_recovery(self):
        board=Board();board.cpu.mem_write(0x900F0000,b"\xff"*8192)
        c=upload(board,payload());c.call("activate",retry=False)
        self.assertEqual(board.call("test_boot"),0)

if __name__ == "__main__":
    unittest.main(verbosity=2)
