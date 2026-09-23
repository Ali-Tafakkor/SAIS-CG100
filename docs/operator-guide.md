# Operator guide

## Prepare

1. Extract the entire Windows ZIP. Keep its directory structure intact.
2. Connect independent board power. A DHCP network is recommended, especially for several boards. A direct cable requires a compatible host subnet; the installer does not silently change Windows networking or firewall rules.
3. For a new board, connect one ST-LINK per board and Ethernet. For a prepared board, Ethernet alone is sufficient for communication.
4. Run `Run-Programmer.cmd`. The UI is local at `127.0.0.1`. Choose **Use included firmware** for the tested offline package, or **Fetch latest firmware** to download a published release.

Keep the console window open while using the installer. It prints the exact local URL. If the default browser does not connect, the launcher tries installed browser executables. You can also paste the printed URL into your browser or open `%LOCALAPPDATA%/SAIS-CG100/Open-Installer.url`. Startup diagnostics are saved to `launcher.log` in that same state directory. If startup fails, the console stays open with the error; no board operation starts automatically.

## Initial setup with Ethernet

Choose **Set up with Ethernet**, scan probes, select boards and inspect them. Review UIDs and type the requested `ERASE <count>` phrase. All firmware and runtime dependencies are validated before erasing. A per-board update key is saved and read back before deletion.

The installer reads both internal Flash and external NOR twice and verifies matching backups. It erases both memories and verifies them blank. It writes a small independent recovery service to NOR, a device access record and empty A/B boot metadata, then writes Stage 0 last. Each written region is read back before the final SWD session releases reset/debug and exits.

From this boundary onward the installer cannot call SWD. It discovers the same UID over Ethernet, authenticates it and transfers the main image in binary blocks up to 16 KiB. It verifies the NOR digest, starts a trial, checks the running image/version/health and confirms the trial over Ethernet. The ST-LINK cable can remain attached without being used; removing it must not remove board power. SWD pins and option bytes are not permanently disabled.

If Ethernet is unavailable after the bootstrap, the recovery service remains available. Fix the cable/routing, choose **Install or update over Ethernet**, discover the board and retry. Do not erase a prepared board just because the network transfer was interrupted.

## Full SWD installation

Choose **Install everything with SWD** when desired. This installs the same network recovery foundation plus the application in A and B. A complete readback remains part of the SWD phase. Runtime checks wait through the recovery window before checking the application. Later updates use the Ethernet route.

## Later installation or update

After successful initial preparation, independent power and Ethernet are sufficient
for application updates. Keep the enrolled Windows account or import the saved
board access file on another computer. Updating Stage 0 or the independent recovery
image is outside this route and still requires a separate SWD service operation.

Choose **Install or update over Ethernet**. Discover boards, or enter a known IPv4 address when broadcast discovery cannot cross your router/VPN. Select registered boards, load firmware and confirm `UPDATE <count>`. This mode does not enumerate ST-LINK probes or execute OpenOCD.

Authenticated updates and their dedicated discovery query do not require the controller's address to appear in the legacy source-IP allow-list. Existing non-update management APIs retain that list. The full SWD route also uses authenticated update status for its final runtime checks.

An interruption during upload leaves the confirmed image intact. Retry through Ethernet. A failed trial gets at most two boot attempts, then returns to the confirmed image. A board with no usable application remains in network recovery. Recovery opens a roughly 20-second window on ordinary resets, even if the confirmed application is unhealthy. Application trial confirmation has a two-minute deadline; repeated failures return to recovery/previous firmware.

## Automatic fleet updates

Enable **Automatic updates** per registered board, then leave `Run-Fleet-Updater.cmd` running on a Windows computer connected to the sites through LAN/VPN. It checks GitHub releases hourly. Use a Windows account that can decrypt the enrolled DPAPI keys. Running the script does not install a Windows service or change startup settings. Closing it stops future checks; manual updates remain available.

A reachable board is updated first. Only after it passes does the worker fan out to the rest. Offline devices are retried later. Firmware/authentication failures hold that release to prevent repeated restart loops. Inspect `%LOCALAPPDATA%/SAIS-CG100/fleet-reports/latest.json`, perform a manual recovery if needed, then use **Resume automatic updates after review**. Enabling unattended updates authorizes board restarts; use an operational maintenance policy appropriate to the equipment.

No cloud relay or inbound Internet exposure is created. A reachable site computer/VPN and running agent are required. Publishing a release does not by itself make an isolated industrial network reachable.

## Access files, backups and reports

- Credentials: `%LOCALAPPDATA%/SAIS-CG100/credentials/<UID>.dpapi`, encrypted for the current Windows user.
- Original verified memory backups and run reports: `%LOCALAPPDATA%/SAIS-CG100/reports/<run-id>/board-<UID>/`.
- `board-access.g100-key` in the initial installation report directory is a portable recovery credential. Store it privately. It and full NOR backups contain the board update key. Do not publish or email them as ordinary logs.
- To use another computer, import this access file in the Ethernet section. The installer stores it in that user's DPAPI vault. Ordinary result JSON and UI state exclude the secret key.
- Settings and configured protocol instances use two NOR records and survive reboot/update. Unconfirmed settings candidates, temporary API operations and existing TLS/credential provisioning remain volatile. Schema compatibility must be preserved by future firmware.

`passed` means the checks for that route completed; it is not physical qualification of a hardware fleet. Full SWD installation can report `partial` when readback passes but Ethernet is unreachable. Network routes report failure when authenticated runtime acceptance cannot be proved. Read the per-board error, not just the progress percentage.

## Performance and qualification

The transport avoids hexadecimal expansion and sends binary blocks up to 16 KiB with readback, retry and digest checks. The UI/report measures actual throughput. Backups and whole-memory blank verification still take time over SWD during initial provisioning. No unconditional Ethernet-versus-SWD speed multiplier is claimed.

The [qualification record](qualification.md) separates software/emulation results from completed physical tests on one board. Before production batches, extend the acceptance procedure in `ethernet-update.md` to your hardware, routed networking, multiple boards and precisely controlled interruption points.

Update traffic is authenticated but not encrypted. Release downloads use certificate-validated HTTPS and manifest hashes; device updates trust the enrolled installer key. Stage 0 uses CRC integrity checks, not a vendor signature. Existing non-update APIs are still development APIs. Use a managed LAN/VPN and complete a production security review before industrial exposure.
