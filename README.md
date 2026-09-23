# SAIS-CG100

Portable Windows installation and Ethernet updates for **MainBoard v2.6 / STM32H750 / W25Q128JV 16 MiB / SDRAM 16 MiB**.

**[Download the published Windows package](https://github.com/Ali-Tafakkor/SAIS-CG100/releases/latest/download/SAIS-CG100-Programmer-win-x64.zip)** | [Operator guide](docs/operator-guide.md) | [Ethernet architecture and API](docs/ethernet-update.md)

Extract the complete ZIP and run **Run-Programmer.cmd**. Python, OpenOCD, the initial recovery image and the main firmware are included. No compiler or Python installation is required. Use the included firmware offline, or fetch a published release over HTTPS. Windows 10/11 x64 is supported.

## Choose an installation route

| Route | Connections | What happens |
| --- | --- | --- |
| Set up with Ethernet | Power, ST-LINK and Ethernet | Verified backup and erase; SWD writes only Stage 0, recovery and board credentials; OpenOCD exits; the main application transfers, boots and is confirmed through Ethernet. |
| Install everything with SWD | Power and ST-LINK; Ethernet for runtime checks | Recovery plus both application slots are written and verified through SWD. Later Ethernet updates are available. |
| Install or update over Ethernet | Independent power and Ethernet | Prepared boards receive their first application or a later release without opening or enumerating an ST-LINK. |
| Erase only | Power and ST-LINK | Verified backup, complete internal/NOR erase and blank verification. |

Select one or several boards, review their UIDs and start. Initial provisioning requires `ERASE <count>`; network installation requires `UPDATE <count>`. Progress includes transfer bytes and measured throughput. Reports distinguish written bytes, runtime checks and failures.

A resident recovery service starts during a rescue window on every ordinary reset. The main firmware uses two application slots, full readback hashing and a trial boot. An authenticated installer confirms the running version, image digest and health before accepting the trial. Unconfirmed trials roll back. Confirmed settings and protocol configuration survive application updates in redundant NOR records.

## Updates after deployment

Register boards and enable **Automatic updates** per board, then run **Run-Fleet-Updater.cmd** on a computer with LAN/VPN access to them. The process checks releases hourly while running, tries one reachable board first, then updates the remaining boards with bounded parallelism. Failed firmware releases are held for review. Offline boards are retried later. The computer and board must stay powered; Ethernet does not imply Internet reachability or power over Ethernet.

Firmware requests and responses use a different HMAC key for each board, replay protection and authenticated image digests. Host keys are protected with Windows DPAPI. This is not a TLS listener or verified secure boot; existing management APIs retain their documented development policy. Read the security and qualification boundaries in the [operator guide](docs/operator-guide.md).

## Qualification status

**v0.2.1** includes firmware **3.3.0-dev-netinstall** and independent recovery **1.0.0-recovery**. Download the new Windows ZIP to obtain the Ethernet routes and corrected launcher; an older installer does not upgrade its own UI automatically.

Both initial installation routes passed on one board. Ethernet testing with SWD physically removed covered a changed version, cold power cycling, 16 invalid-request/corrupt-image checks, unconfirmed-trial rollback, recovery after an interrupted transfer and power loss, and a maximum-size image. Host tests, actual ARM firmware logic under emulation and packaged Windows startup checks also pass. See the [qualification record](docs/qualification.md) for measured throughput and the limits of this evidence. Multiple-board and broader deployment testing remain necessary before production use.

## Source map

For a new development session, start with the [continuation handoff](docs/continuation.md).

- `installer/`: UI, SWD provisioning, authenticated Ethernet client, credentials, fleet worker and tests.
- `SetAPIs_Calling/firmware/app/`: application, recovery API, update service and authentication.
- `firmware/bootloader/`: small internal Stage 0.
- `firmware/memory-platform/`: boot selection, memory drivers and redundant configuration storage.
- `tools/test_firmware_arm.py`: actual C firmware logic executed on emulated ARM with NOR faults.
- `tools/make_release.py`, `tools/build-package.ps1`: firmware assets and standalone Windows ZIP.
- `.github/workflows/release.yml`: build/test/release on a version tag.

CMSIS, HAL, LwIP, Mbed TLS, Python and OpenOCD license files remain alongside their distributions. See [development instructions](docs/development.md).
