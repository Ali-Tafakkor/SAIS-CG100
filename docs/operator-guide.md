# Operator guide

## Hardware and host

- Windows 10/11 x64 computer with Internet access and at least 110 MiB of free working space per board.
- MainBoard v2.6 with STM32H750, W25Q128JV 16 MiB external NOR and 16 MiB SDRAM. Other revisions are not supported by this programmer.
- One USB ST-LINK/SWD connection per board, plus board power. Use separate ST-LINK probes with distinct USB serial numbers.
- Ethernet connection to the same routed network as the computer if Ethernet runtime checks are required. DHCP is recommended for a batch. A direct-link fallback address is derived from the MCU UID, but a short IPv4 host number can collide across a large batch.

The programmer uses bundled OpenOCD and libusb. If the scan finds no ST-LINK, inspect the USB cable, board power, SWD wiring and driver. Keep other programming/debugging tools closed during a run.

## Start a run

1. Download the complete ZIP from the release link on the repository home page. Extract it; do not run the CMD file from inside the ZIP viewer.
2. Double-click `Run-Programmer.cmd`. A local browser page opens at `127.0.0.1`. Keep the launcher running until the batch completes.
3. Select **Scan ST-LINK probes** and **Fetch latest firmware**. For **Erase only**, downloading firmware is optional.
4. Select the desired probes and choose **Inspect selected boards**. Review each full MCU UID, current internal Flash state, three NOR image headers and boot metadata. An inspection error blocks that board.
5. Choose **Erase and install** or **Erase only**, and set parallel workers. Press **Review and start**, review the full UIDs, then type the displayed confirmation phrase. The app does not erase before this confirmation.
6. Wait for all boards. Do not disconnect a board during backup, erase, programming or readback. Save the JSON summary and keep the per-board backup directories.

The initial estimated time is approximate. A complete 16 MiB NOR read over SWD is slow; two backup reads and full post-write checks can take several minutes per board. Parallel workers reduce batch wall time only when USB/host resources permit it.

If **Fetch latest firmware** fails, no board has been changed. The programmer downloads the latest release manifest from `github.com`, then the binaries for that exact release tag, and verifies their lengths and SHA-256 hashes. It automatically tries Python HTTPS, Windows curl and PowerShell for connection failures. If all fail, check access to `github.com` and `release-assets.githubusercontent.com`, including any local proxy or firewall, and press **Fetch latest firmware** again. TLS certificate checks stay enabled.

## Results

| Result | Meaning |
| --- | --- |
| `passed` | Erase-only: both memories read completely blank. Install: full readback and Ethernet runtime checks passed. |
| `partial` | Install bytes passed full readback, but no matching board answered Ethernet discovery. Check routing, DHCP and link, then retest the network. |
| `failed` | A hardware, download, checksum, erase, readback or runtime check failed. Review the board's log and JSON report before retrying. |

Each selected board receives an independent directory under `%LOCALAPPDATA%\SAIS-CG100\reports\<run-id>\board-<UID>`. The `verified-backup.json` file records the two matching reads and SHA-256 values. `backup-internal.bin` and `backup-nor.bin` are the original onboard contents. `postwrite-*` files and OpenOCD logs provide verification evidence. Keep backups outside the public repository.

Erase-only removes Stage 0 and every byte in internal Flash and external NOR. The board cannot boot until it is programmed again. This operation does not modify the MCU UID, OTP, option bytes, host network settings, SD card or other storage outside the stated onboard memories.

## Qualification boundary

Read-only discovery and source builds have been verified. Full erase, initial installation and concurrent multi-board programming need an expendable-board qualification run before production batches. Firmware security remains in development mode: management APIs are open, settings are volatile and the boot image is integrity-checked but not authenticated.
