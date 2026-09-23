# Development and releases

`installer/` uses only the Python standard library. The operator ZIP includes Python 3.13.14 and xPack OpenOCD 0.12.0-7. Its local browser UI binds only to `127.0.0.1`; state-changing requests require a per-process token. The firmware downloader retrieves the latest GitHub release manifest and binaries via HTTPS, checks SHA-256 and lengths, then validates the SDRAM image ABI.

## Firmware build

The source tree uses the same layout as the tested G100 project. `SetAPIs_Calling/scripts/build.ps1 -Profile shadow` builds the SDRAM application; `-Profile recovery` builds the independent network bootstrap. `python scripts/architecture/build-boot.py` builds Stage 0. The builders accept `G100_COMPILER_BIN` pointing to the xPack GCC `bin` directory, with project-local and sibling-project fallbacks for development. Generated build directories are ignored by Git. See the [Ethernet architecture](ethernet-update.md) for update commands and limits.

The release workflow downloads the pinned compiler, OpenOCD and embedded Python archives listed in `tools/toolchain-manifest.json` and verifies their SHA-256 values. `tools/make_release.py` rejects a failed or mismatched source build and records whether the source tree has uncommitted changes. `tools/build-package.ps1` stages the portable ZIP with Python, OpenOCD scripts/libraries/licenses and the local UI. A published `vMAJOR.MINOR.PATCH` tag triggers `.github/workflows/release.yml`; it compiles, runs tests and publishes five release assets:

- `SAIS-CG100-Programmer-win-x64.zip`
- `firmware-manifest.json`
- `g100-api.bin`
- `stage0.bin`
- `recovery.bin`

The stable ZIP name makes the repository home page's `releases/latest/download/...` link work across versions. The operator can select included firmware offline or fetch the latest published firmware. A protocol-compatible firmware update does not require replacing the operator ZIP. Installer/protocol upgrades require the appropriate new package.

## Hardware protocol

The 96-bit MCU UID is the canonical identity. The application derives a full-UID label and hostname and an FNV-based short fallback IPv4 host number. The 8-bit legacy `slot` API field is a hint and is not guaranteed unique. ST-LINKs are selected by their raw USB serial descriptors, never by discovery order. An OpenOCD UID guard and MCU revision/Flash-size check run before every operation; inspection also verifies the Winbond NOR ID and 16 MiB capacity.

For initial setup, each board receives two complete matching backup reads, full NOR/internal Flash erase and full blank readback. The Ethernet route writes recovery, credentials and empty A/B metadata before Stage 0, verifies written regions and closes SWD before transferring the application. The full SWD route also writes A/B and performs a complete final readback before Ethernet runtime checks. Stage 0 is written last so an incomplete external bootstrap cannot be booted through a new bootloader. Prepared boards use only authenticated Ethernet. No option bytes, OTP, SD or host NIC settings are touched.

The package is a development/manufacturing pilot, not a production security boundary. A later production release should add vendor-signed secure boot, authenticated management, physical power-loss tests and broad hardware qualification.


## Network installer (protocol 2)

Build all three assets before packaging:

```powershell
./SetAPIs_Calling/scripts/build.ps1 -Profile shadow
./SetAPIs_Calling/scripts/build.ps1 -Profile recovery
python scripts/architecture/build-boot.py
python -m unittest discover -s installer/tests -v
python -m pip install --target .cache/test-deps unicorn==2.1.4
python tools/test_firmware_arm.py
python tools/check_public.py
node --check installer/web/app.js
python tools/make_release.py v0.2.1 dist/release
```

Then invoke `tools/build-package.ps1` with the pinned embedded Python ZIP and OpenOCD runtime. It includes `firmware-release/`, the UI, credentials support and `Run-Fleet-Updater.cmd`. Nothing needs to be downloaded during initial installation when using included firmware. The release workflow publishes `recovery.bin` as a separate immutable asset and runs host plus ARM firmware tests first.

Building a local ZIP does not publish a tag or release. See `qualification.md` for completed hardware tests and `ethernet-update.md` for the remaining deployment acceptance procedure. Do not ship board keys, backups or `.local-test/` files.
