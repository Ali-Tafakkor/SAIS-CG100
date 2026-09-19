# Development and releases

`installer/` uses only the Python standard library. The operator ZIP includes Python 3.13.14 and xPack OpenOCD 0.12.0-7. Its local browser UI binds only to `127.0.0.1`; state-changing requests require a per-process token. The firmware downloader retrieves the latest GitHub release manifest and binaries via HTTPS, checks SHA-256 and lengths, then validates the SDRAM image ABI.

## Firmware build

The source tree uses the same layout as the tested G100 project. `SetAPIs_Calling/scripts/build.ps1 -Profile shadow` builds the SDRAM application. `python scripts/architecture/build-boot.py` builds Stage 0. Both accept `G100_COMPILER_BIN` pointing to the xPack GCC `bin` directory; otherwise they use a project-local `toolchain/xpack-arm-none-eabi-gcc-15.2.1-1.1/bin` path. Generated build directories are ignored by Git.

The release workflow downloads the pinned compiler, OpenOCD and embedded Python archives listed in `tools/toolchain-manifest.json` and verifies their SHA-256 values. `tools/make_release.py` rejects a failed or mismatched source build. `tools/build-package.ps1` stages the portable ZIP with Python, OpenOCD scripts/libraries/licenses and the local UI. Push a `vMAJOR.MINOR.PATCH` tag to trigger `.github/workflows/release.yml`; it compiles, runs tests and publishes four release assets:

- `SAIS-CG100-Programmer-win-x64.zip`
- `firmware-manifest.json`
- `g100-api.bin`
- `stage0.bin`

The stable ZIP name makes the repository home page's `releases/latest/download/...` link work across versions. The operator app checks GitHub's latest release on every installation run, so updating a firmware release does not require replacing an already downloaded operator ZIP. Package upgrades can be made by downloading the newest ZIP.

## Hardware protocol

The 96-bit MCU UID is the canonical identity. The application derives a full-UID label and hostname and an FNV-based short fallback IPv4 host number. The 8-bit legacy `slot` API field is a hint and is not guaranteed unique. ST-LINKs are selected by their raw USB serial descriptors, never by discovery order. An OpenOCD UID guard and MCU revision/Flash-size check run before every operation; inspection also verifies the Winbond NOR ID and 16 MiB capacity.

For each board, the write sequence is: two complete matching backup reads, full NOR erase, internal Flash erase, full blank readback, factory/A/B image writes and verification, two metadata commits, Stage 0 internal write and verification, full final readback, then Ethernet runtime tests. Stage 0 is written last so a partial external image cannot be booted through a new bootloader. The backup remains available if any step fails. No option bytes, OTP, SD or host NIC settings are touched.

The package is a development/manufacturing pilot, not a production security boundary. A later production release should add signed firmware, authenticated management, persistent configuration, power-loss tests and broad hardware qualification.
