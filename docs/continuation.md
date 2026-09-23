# Development handoff - 2026-09-23

## Completed baseline

The network installation/update milestone is complete for the single tested
MainBoard v2.6 / STM32H750 with 16 MiB NOR and 16 MiB SDRAM. The released Windows
installer is [v0.2.1](https://github.com/Ali-Tafakkor/SAIS-CG100/releases/tag/v0.2.1),
with application `3.3.0-dev-netinstall`, recovery `1.0.0-recovery` and update
protocol 2. Release source: `1e2613149a1faec1e91d73306c378000992bd602`.
Documentation commits after that revision do not rebuild or replace its assets.

Read [qualification](qualification.md) for evidence, [operator guide](operator-guide.md)
for operation, [architecture/API](ethernet-update.md) for update invariants, and
[development](development.md) for build/release commands. On the original workstation,
the parent project's `PROJECT_HANDOFF.md` contains the private board identity,
last observed state and evidence paths. Its `manufacturing/` clone is the source
of this release; older parent-project firmware copies are historical baselines.

## Decisions to preserve

- Prepared boards require independent power, Ethernet reachability and their
  enrolled access key for ordinary application updates. No ST-LINK enumeration
  or OpenOCD process belongs in that path, including retries and recovery.
- Firmware is compiled on the host and transferred as a complete application
  image. This is not a general source-file or filesystem upload service.
- Initial provisioning offers either SWD bootstrap followed by Ethernet main
  installation, or complete SWD installation. Both retain Ethernet updates.
- The bootstrap includes Stage 0, independent NOR recovery and per-board access
  data. Stage 0 alone cannot provide Ethernet. Normal updates preserve those
  regions and confirmed configuration; they write the inactive A/B slot.
- SWD detach means releasing debug/reset and closing the programming process.
  It does not permanently disable pins, lock option bytes or burn security fuses.
- Changing or repairing Stage 0/recovery is a service operation; the ordinary
  network update API cannot rewrite them. Do not erase a prepared board to solve
  a routine connection or interrupted-upload problem.
- The Windows account retains its board key through DPAPI. A different account
  or computer imports the privately saved `board-access.g100-key`. Keys and full
  NOR backups must stay outside the public repository and release.
- Automatic updates are opt-in and require a running `Run-Fleet-Updater.cmd`
  process on a reachable LAN/VPN computer. No service, startup task or cloud
  relay is installed. Publishing a release alone does not reach isolated sites.

## Work and verification completed

The browser launcher now reports errors, prints a usable local URL and handles
default-browser dispatch failures. An in-page review dialog replaces unsupported
native prompts. UI reload restores the running route and board selection.
Cross-process locks protect board operations and credential imports.

Both initial routes passed with matching full backups, erase/blank verification
and authenticated runtime acceptance. With SWD physically removed, a distinct
firmware version, cold power cycle, 16 admission/corruption checks, two unconfirmed
trial boots followed by rollback, power loss with incomplete transfer, and a
maximum-capacity image all passed. Separately, 31 host tests and 13 compiled-ARM
simulation tests passed. See qualification for each test's actual limits.

The first v0.2.0 tag failed its CI ARM test setup because a compiler-path variable
did not persist between steps. No binary release was created for that tag.
v0.2.1 sets the compiler path in the test step. Actions run `35862788051` passed,
and the public ZIP was downloaded, hash-checked, launched and used for a successful
Ethernet application update with authenticated digest/health/boot confirmation.

Absolute source paths in lwIP assertion strings explain the original local/CI
binary differences. Rebuilding locally with only those macro paths mapped to the
CI checkout reproduced both published main and recovery binaries exactly.
Stage 0 already matched. Compare a board against its selected manifest, not an
older locally built image's hash just because the version text is identical.

## Measured performance and open limits

The downloaded 134,824-byte application transferred in 1.860 seconds; the complete
network operation took 13.259 seconds. A 4,190,208-byte padded image transferred
in 53.408 seconds. The link negotiated 100 Mbps; that is not measured payload
throughput. No same-image, same-conditions SWD/Ethernet speed comparison was made.
The operator explicitly declined an additional speed comparison; do not schedule
one as unfinished work.

The release remains a development/manufacturing pilot. Multiple-board physical
qualification, routed/DHCP deployments, deliberate CPU crashes, precise power cuts
during commits and production secure boot remain separate future work. Existing
protocol configuration APIs do not establish working Modbus/BACnet/KNX/MQTT
drivers or a Flow engine. Update authentication is not encryption or vendor-signed
secure boot.

## Continuation boundary

The latest request was documentation and handoff to another agent. There is no
unfinished installation or mandatory repeat hardware test. Choose the next
development scope with the operator. Do not start another erase, SWD session,
power-cycle request, speed benchmark or deployment merely from this document.
For a later authorized board task, recheck identity and authenticated Ethernet
state; recorded IPs, generations, process IDs and UI ports are observations, not
promises about a new session. Preserve existing backups and configuration.
