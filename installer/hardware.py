"""ST-LINK enumeration and guarded OpenOCD access for MainBoard v2.6."""

from __future__ import annotations

import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OPENOCD_ROOT = Path(os.environ.get("G100_OPENOCD_ROOT", ROOT / "runtime" / "openocd"))
STLINK_PIDS = {0x3744, 0x3748, 0x374B, 0x374D, 0x374E, 0x374F,
              0x3752, 0x3753, 0x3754, 0x3755, 0x3757}
INTERNAL_BYTES = 0x20000
NOR_BYTES = 0x1000000
IMAGE_OFFSETS = (0x100000, 0x500000, 0x10000)
META_OFFSETS = (0xF0000, 0xF1000)


class HardwareError(RuntimeError):
    pass


class _USBDescriptor(ct.Structure):
    _fields_ = [("bLength", ct.c_uint8), ("bDescriptorType", ct.c_uint8),
                ("bcdUSB", ct.c_uint16), ("bDeviceClass", ct.c_uint8),
                ("bDeviceSubClass", ct.c_uint8), ("bDeviceProtocol", ct.c_uint8),
                ("bMaxPacketSize0", ct.c_uint8), ("idVendor", ct.c_uint16),
                ("idProduct", ct.c_uint16), ("bcdDevice", ct.c_uint16),
                ("iManufacturer", ct.c_uint8), ("iProduct", ct.c_uint8),
                ("iSerialNumber", ct.c_uint8), ("bNumConfigurations", ct.c_uint8)]


def list_probes() -> list[dict]:
    """Read USB string descriptors; never select a probe by enumeration order."""
    if sys.platform != "win32":
        raise HardwareError("The programmer supports Windows 10/11 only")
    dll = OPENOCD_ROOT / "bin" / "libusb-1.0.dll"
    if not dll.is_file():
        raise HardwareError(f"Bundled USB driver library is missing: {dll}")
    usb = ct.CDLL(str(dll))
    usb.libusb_init.argtypes = [ct.POINTER(ct.c_void_p)]
    usb.libusb_init.restype = ct.c_int
    usb.libusb_get_device_list.argtypes = [ct.c_void_p, ct.POINTER(ct.POINTER(ct.c_void_p))]
    usb.libusb_get_device_list.restype = ct.c_ssize_t
    usb.libusb_get_device_descriptor.argtypes = [ct.c_void_p, ct.POINTER(_USBDescriptor)]
    usb.libusb_get_device_descriptor.restype = ct.c_int
    usb.libusb_open.argtypes = [ct.c_void_p, ct.POINTER(ct.c_void_p)]
    usb.libusb_open.restype = ct.c_int
    usb.libusb_get_string_descriptor_ascii.argtypes = [ct.c_void_p, ct.c_uint8,
                                                        ct.POINTER(ct.c_ubyte), ct.c_int]
    usb.libusb_get_string_descriptor_ascii.restype = ct.c_int
    usb.libusb_close.argtypes = [ct.c_void_p]
    usb.libusb_free_device_list.argtypes = [ct.POINTER(ct.c_void_p), ct.c_int]
    usb.libusb_exit.argtypes = [ct.c_void_p]
    context = ct.c_void_p()
    if usb.libusb_init(ct.byref(context)):
        raise HardwareError("Cannot initialize the bundled USB library")
    devices = ct.POINTER(ct.c_void_p)()
    found = []
    try:
        count = usb.libusb_get_device_list(context, ct.byref(devices))
        if count < 0:
            raise HardwareError(f"USB enumeration failed ({count})")
        try:
            for index in range(count):
                descriptor = _USBDescriptor()
                if usb.libusb_get_device_descriptor(devices[index], ct.byref(descriptor)):
                    continue
                if descriptor.idVendor != 0x0483 or descriptor.idProduct not in STLINK_PIDS:
                    continue
                handle = ct.c_void_p()
                if usb.libusb_open(devices[index], ct.byref(handle)):
                    continue
                try:
                    raw = (ct.c_ubyte * 256)()
                    length = usb.libusb_get_string_descriptor_ascii(
                        handle, descriptor.iSerialNumber, raw, len(raw))
                    if length <= 0:
                        continue
                    serial = bytes(raw[:length])
                    found.append({"serial": serial.hex().upper(),
                                  "usb_product_id": f"{descriptor.idProduct:04X}"})
                finally:
                    usb.libusb_close(handle)
        finally:
            usb.libusb_free_device_list(devices, 1)
    finally:
        usb.libusb_exit(context)
    if len({p["serial"] for p in found}) != len(found):
        raise HardwareError("Duplicate ST-LINK serial numbers; parallel operation is unsafe")
    return sorted(found, key=lambda p: p["serial"])


def _tcl_path(path: Path) -> str:
    value = path.resolve().as_posix()
    if any(ch in value for ch in "{}\n\r"):
        raise HardwareError("Unsafe path for OpenOCD")
    return "{" + value + "}"


def uid_words(uid: str) -> str:
    if not re.fullmatch(r"[0-9A-Fa-f]{24}", uid):
        raise HardwareError("Invalid MCU UID")
    return " ".join("0x" + uid[i:i + 8].upper() for i in (0, 8, 16))


def uid_guard(uid: str) -> str:
    return f"set G100_EXPECTED_UID {{{uid_words(uid)}}}\ng100_target_check\n"


def run_openocd(serial: str, commands: str, directory: Path,
                label: str, timeout: int = 1200, reset: bool = True, detach: bool = False) -> str:
    if not re.fullmatch(r"(?:[0-9A-F]{2}){1,128}", serial):
        raise HardwareError("Invalid ST-LINK serial")
    directory.mkdir(parents=True, exist_ok=True)
    executable = OPENOCD_ROOT / "bin" / "openocd.exe"
    scripts = OPENOCD_ROOT / "openocd" / "scripts"
    if not executable.is_file() or not scripts.is_dir():
        raise HardwareError("The bundled OpenOCD runtime is incomplete")
    if not (ROOT / "scripts" / "stlink-h750.cfg").is_file():
        raise HardwareError("The board configuration is missing")
    script = directory / f"{label}.tcl"
    script.write_text("init\nreset halt\n" + commands +
                      ("reset run\n" if reset else "") +
                      ("mww 0x5C001054 0\nmww 0xE000EDF0 0xA05F0000\n" if detach else "") +
                      "echo G100_OPERATION_COMPLETE\nshutdown\n", encoding="ascii")
    # ST-LINK serials may contain binary bytes. Tcl hex escapes preserve them.
    selector = 'adapter serial "' + ''.join(
        f"\\x{byte:02x}" for byte in bytes.fromhex(serial)) + '"'
    command = [str(executable), "-s", str(scripts), "-c", selector,
               "-f", str(ROOT / "installer" / "hardware" / "board.tcl"),
               "-f", str(script)]
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                errors="replace", timeout=timeout,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except subprocess.TimeoutExpired as exc:
        raise HardwareError(f"{label} timed out after {timeout} seconds") from exc
    output = result.stdout + result.stderr
    (directory / f"{label}.log").write_text(output, encoding="utf-8")
    if result.returncode or "G100_OPERATION_COMPLETE" not in output:
        raise HardwareError(f"{label} failed; see {directory / (label + '.log')}")
    return output


def inspect(serial: str, directory: Path) -> dict:
    header_files = [directory / f"slot{slot}-header.bin" for slot in range(3)]
    commands = "g100_hardware_check\n"
    commands += "set u [read_memory 0x1ff1e800 32 3]\n"
    commands += 'echo [format "G100_UID=%08X%08X%08X" [lindex $u 0] [lindex $u 1] [lindex $u 2]]\n'
    commands += f"dump_image {_tcl_path(directory / 'internal-head.bin')} 0x08000000 8192\n"
    commands += "g100_qspi_read_map\nflash info 1\n"
    for slot, (offset, target) in enumerate(zip(IMAGE_OFFSETS, header_files)):
        commands += f"flash read_bank 1 {_tcl_path(target)} {offset} 4096\n"
    for number, offset in enumerate(META_OFFSETS):
        commands += f"flash read_bank 1 {_tcl_path(directory / f'meta{number}.bin')} {offset} 4096\n"
    output = run_openocd(serial, commands, directory, "inspect", timeout=180)
    if not re.search(r"flash1 .*id = 0x1840ef size = 16384 KiB", output):
        raise HardwareError("The external NOR is not the expected 16 MiB Winbond W25Q128JV")
    match = re.search(r"G100_UID=([0-9A-F]{24})", output)
    if not match:
        raise HardwareError("OpenOCD did not return a 96-bit MCU UID")
    uid = match.group(1)
    internal = (directory / "internal-head.bin").read_bytes()
    if len(internal) != 8192:
        raise HardwareError("Incomplete internal Flash inspection")
    images = []
    for slot, path in enumerate(header_files):
        raw = path.read_bytes()
        if len(raw) != 4096:
            raise HardwareError("Incomplete external NOR inspection")
        if all(byte == 0xFF for byte in raw):
            images.append({"slot": slot, "state": "blank"})
        elif len(raw) >= 128 and struct.unpack_from("<I", raw)[0] == 0x314D4947:
            import zlib
            header_valid = zlib.crc32(raw[:124]) == struct.unpack_from("<I", raw, 124)[0]
            images.append({"slot": slot, "state": "G100 image" if header_valid else "damaged image header",
                           "generation": struct.unpack_from("<I", raw, 24)[0],
                           "payload_bytes": struct.unpack_from("<I", raw, 16)[0],
                           "payload_sha256": raw[32:64].hex()})
        else:
            images.append({"slot": slot, "state": "other data"})
    from image_format import meta_unpack
    metadata = [meta_unpack((directory / f"meta{i}.bin").read_bytes()) for i in range(2)]
    return {"uid": uid, "probe_serial": serial,
            "internal_flash": "blank" if all(b == 0xFF for b in internal) else "programmed",
            "internal_head_sha256": hashlib.sha256(internal).hexdigest(),
            "images": images, "metadata": metadata,
            "hardware": "MainBoard-v2.6 / STM32H750 / W25Q128JV expected"}


def read_all(serial: str, uid: str, directory: Path, prefix: str) -> dict[str, Path]:
    internal = directory / f"{prefix}-internal.bin"
    nor = directory / f"{prefix}-nor.bin"
    commands = uid_guard(uid)
    commands += f"dump_image {_tcl_path(internal)} 0x08000000 {INTERNAL_BYTES}\n"
    commands += "g100_qspi_read_map\n"
    commands += f"flash read_bank 1 {_tcl_path(nor)} 0 {NOR_BYTES}\n"
    run_openocd(serial, commands, directory, prefix, timeout=1800, detach=True)
    if internal.stat().st_size != INTERNAL_BYTES or nor.stat().st_size != NOR_BYTES:
        raise HardwareError("Full onboard Flash read was incomplete")
    return {"internal": internal, "nor": nor}


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def full_backup(serial: str, uid: str, directory: Path) -> dict:
    first = read_all(serial, uid, directory, "backup")
    second = read_all(serial, uid, directory, "backup-reread")
    files = {}
    for key in ("internal", "nor"):
        left, right = first[key], second[key]
        checksum = file_hash(left)
        if checksum != file_hash(right):
            raise HardwareError(f"Independent {key} backup reads disagree")
        files[key] = {"path": str(left), "bytes": left.stat().st_size, "sha256": checksum}
        right.unlink()
    record = {"verified": True, "uid": uid, "probe_serial": serial, "files": files}
    (directory / "verified-backup.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    return record


def _qspi_writer() -> str:
    return ("g100_qspi_read_map\n"
            "stmqspi set 1 w25q128jv-4k 0x1000000 0x100 0x03 0x0b 0x02 0xc7 0x1000 0x20\n")


def erase_all(serial: str, uid: str, directory: Path) -> None:
    commands = uid_guard(uid) + _qspi_writer()
    commands += "stmqspi mass_erase 1\n"
    # Keep Stage 0 intact until external NOR has been erased successfully.
    commands += "flash erase_address 0x08000000 0x20000\n"
    run_openocd(serial, commands, directory, "erase", timeout=3600)


def _write_image(directory: Path, slot: int, image: bytes) -> str:
    offset = IMAGE_OFFSETS[slot]
    base = directory / f"slot{slot}"
    payload = base.with_name(base.name + "-payload.bin")
    body = base.with_name(base.name + "-header.bin")
    magic = base.with_name(base.name + "-commit.bin")
    whole = base.with_name(base.name + "-image.bin")
    payload.write_bytes(image[4096:])
    body.write_bytes(image[4:128])
    magic.write_bytes(image[:4])
    whole.write_bytes(image)
    return (f"flash write_bank 1 {_tcl_path(payload)} {offset + 4096}\n"
            f"flash verify_bank 1 {_tcl_path(payload)} {offset + 4096}\n"
            f"flash write_bank 1 {_tcl_path(body)} {offset + 4}\n"
            f"flash verify_bank 1 {_tcl_path(body)} {offset + 4}\n"
            f"flash write_bank 1 {_tcl_path(magic)} {offset}\n"
            f"flash verify_bank 1 {_tcl_path(whole)} {offset}\n")


def _write_metadata(directory: Path, index: int, raw: bytes) -> str:
    offset = META_OFFSETS[index]
    base = directory / f"metadata{index}"
    body = base.with_name(base.name + "-body.bin")
    magic = base.with_name(base.name + "-commit.bin")
    whole = base.with_name(base.name + "-whole.bin")
    body.write_bytes(raw[4:])
    magic.write_bytes(raw[:4])
    whole.write_bytes(raw)
    return (f"flash write_bank 1 {_tcl_path(body)} {offset + 4}\n"
            f"flash verify_bank 1 {_tcl_path(body)} {offset + 4}\n"
            f"flash write_bank 1 {_tcl_path(magic)} {offset}\n"
            f"flash verify_bank 1 {_tcl_path(whole)} {offset}\n")


def prepare_install(directory: Path, app: bytes, stage0: bytes, *, recovery: bytes | None = None,
                    provision: bytes | None = None, bootstrap_only: bool = False) -> dict:
    from image_format import pack, meta_pack, FACTORY_BYTES, HEADER
    if not 1024 < len(stage0) < INTERNAL_BYTES:
        raise HardwareError("Invalid Stage 0 image")
    from image_format import NONE
    if bootstrap_only and (recovery is None or provision is None):
        raise HardwareError("Network installation requires a recovery image and device key")
    image = pack(app, 1) if not bootstrap_only else None
    factory = pack(recovery if recovery is not None else app, 1, FACTORY_BYTES - HEADER)
    images = {2: factory} if bootstrap_only else {2: factory, 0: image, 1: image}
    metadata = tuple(meta_pack(n, NONE if bootstrap_only else 0, 0 if bootstrap_only else 1) for n in (1, 2))
    if provision is not None:
        import zlib
        if len(provision) != 64 or zlib.crc32(provision[:60]) != struct.unpack_from("<I", provision, 60)[0]:
            raise HardwareError("Invalid device credential record")
    expected_internal = stage0 + b"\xff" * (INTERNAL_BYTES - len(stage0))
    expected_nor = bytearray(b"\xff" * NOR_BYTES)
    for slot, value in images.items():
        offset = IMAGE_OFFSETS[slot]
        expected_nor[offset:offset + len(value)] = value
    if provision is not None:
        expected_nor[0xE0000:0xE0040] = provision
    for offset, value in zip(META_OFFSETS, metadata):
        expected_nor[offset:offset + len(value)] = value
    (directory / "expected-internal.bin").write_bytes(expected_internal)
    (directory / "expected-nor.bin").write_bytes(expected_nor)
    return {"image": image, "images": images, "provision": provision, "bootstrap_only": bootstrap_only, "metadata": metadata,
            "internal_sha256": hashlib.sha256(expected_internal).hexdigest(),
            "nor_sha256": hashlib.sha256(expected_nor).hexdigest()}


def program(serial: str, uid: str, directory: Path, plan: dict, stage0: Path) -> None:
    commands = uid_guard(uid) + _qspi_writer()
    for slot, image in plan["images"].items():
        commands += _write_image(directory, slot, image)
    if plan.get("provision") is not None:
        record = directory / "board-access.g100-key"
        record.write_bytes(plan["provision"])
        commands += f"flash write_bank 1 {_tcl_path(record)} 917504\n"
        commands += f"flash verify_bank 1 {_tcl_path(record)} 917504\n"
    for index, raw in enumerate(plan["metadata"]):
        commands += _write_metadata(directory, index, raw)
    # Stage 0 is committed only after the selected layout and device key verify.
    commands += f"flash write_image erase {_tcl_path(stage0)} 0x08000000 bin\n"
    commands += f"verify_image {_tcl_path(stage0)} 0x08000000 bin\n"
    # End the final SWD session here: release reset, clear debugger watchdog freeze,
    # and disable core debug. OpenOCD exits before the caller may open Ethernet.
    run_openocd(serial, commands, directory, "program", timeout=1800, detach=True)


def verify_post_write(serial: str, uid: str, directory: Path,
                      expected: dict[str, str] | None = None) -> dict:
    paths = read_all(serial, uid, directory, "postwrite")
    result = {key: {"bytes": path.stat().st_size, "sha256": file_hash(path)}
              for key, path in paths.items()}
    if expected:
        for key in ("internal", "nor"):
            if result[key]["sha256"] != expected[f"{key}_sha256"]:
                raise HardwareError(f"Full {key} readback differs from the expected image")
    else:
        for key, path in paths.items():
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    if any(byte != 0xFF for byte in chunk):
                        raise HardwareError(f"{key} still contains programmed bytes")
    return result
