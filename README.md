# SAIS-CG100

Portable Windows provisioning for the **MainBoard v2.6 / STM32H750 / 16 MiB W25Q128JV / 16 MiB SDRAM** G100 development board.

**[Download the latest Windows programmer](https://github.com/Ali-Tafakkor/SAIS-CG100/releases/latest/download/SAIS-CG100-Programmer-win-x64.zip)** · [View releases](https://github.com/Ali-Tafakkor/SAIS-CG100/releases) · [Operator guide](docs/operator-guide.md)

The download is a ZIP, not an installer. Extract the entire folder and double-click **`Run-Programmer.cmd`**. The local web interface opens in your browser. No Python, compiler or OpenOCD installation is required on the operator's computer. The package contains a pinned Python runtime and OpenOCD; it downloads the newest published firmware release over HTTPS when an install is selected. Windows 10/11 x64 is the supported host.

## Operator flow

1. Supply power to each board. Attach **one ST-LINK per board** by USB/SWD. Connect Ethernet for runtime checks and provide Internet access to this computer.
2. Launch the programmer. Scan probes, fetch the latest firmware, select boards and inspect their MCU UIDs and existing memory contents.
3. Select **Erase and install** or **Erase only**. Review the exact UIDs. The interface requires a second typed confirmation before it starts.
4. The programmer independently backs up all 128 KiB of internal Flash and all 16 MiB of external NOR twice per board, verifies the reads match, erases both memories, and verifies that they are blank.
5. Install mode writes factory, A and B application images and boot metadata to NOR, then writes Stage 0 to internal Flash last. It verifies each write and makes a full final readback. Ethernet checks confirm UID, version, health, architecture, boot and SDRAM status when the network is reachable.
6. Download the summary report in the interface. Detailed logs, readbacks and the verified original backup are stored under `%LOCALAPPDATA%\SAIS-CG100\reports\<run-id>`.

Multiple selected probes run in parallel up to the configured worker limit. Every OpenOCD invocation selects its probe by the USB serial descriptor and rechecks the MCU UID before a write. Erase-only intentionally leaves the board without bootable firmware. MCU UID, OTP and option bytes are never erased.

**Qualification status:** Firmware `3.1.3-dev-sdram` compiles from this source. USB serial selection and read-only inspection have been exercised on one connected Board02. Full erase, initial installation and simultaneous multi-board operation still require validation on expendable boards before production use. The current firmware exposes development APIs and stores settings in volatile RAM; this is not a security-hardened product image. A missing Ethernet route is reported as a partial result, never as a passed runtime test.

## Repository map

| Path | Purpose |
| --- | --- |
| [`installer/`](installer/) | Local browser UI, release client, ST-LINK/OpenOCD hardware layer, parallel workflow and tests. |
| [`SetAPIs_Calling/firmware/app/`](SetAPIs_Calling/firmware/app/) | G100 application; device label, hostname and fallback address derive from the full silicon UID. |
| [`firmware/bootloader/`](firmware/bootloader/) | Stage 0 boot source and linker scripts. |
| [`firmware/memory-platform/`](firmware/memory-platform/) | NOR/SDRAM initialization, image format and boot state. |
| [`tools/`](tools/) | Release asset and portable ZIP builders. |
| [`.github/workflows/release.yml`](.github/workflows/release.yml) | Build, test and publish release assets from a version tag. |

The operator package downloads **prebuilt firmware assets** from the latest GitHub release. It verifies image lengths and SHA-256 values against the release manifest and checks the image ABI before any write. Firmware build dependencies remain in the release workflow, so each operator computer only needs the ZIP, Internet and hardware connections.

The source layout retains the existing STM32 build paths. Third-party CMSIS, HAL, LwIP and Mbed TLS license files remain alongside their source. The portable release contains OpenOCD and Python license files. See [development and release instructions](docs/development.md).
