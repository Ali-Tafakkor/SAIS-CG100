# Network installer qualification

## Test environment

The 2026-09-23 local qualification uses one MainBoard v2.6 with STM32H750,
16 MiB Winbond NOR, independent power and direct Ethernet to a Windows laptop.
The negotiated Ethernet link is 100 Mbps. Device credentials, unique identifiers,
backups and detailed local reports are private and are excluded from the release.

## Verified results

| Test | Evidence / result |
| --- | --- |
| Windows launcher | Actual packaged CMD and project launcher started from another directory; UI, assets and state responded; automatic browser opening confirmed by the operator. |
| Installer review form | Replaced native browser prompt with an in-page dialog. Incorrect text kept Start disabled; Cancel started no operation; correct review completed an Ethernet-only installation and displayed a passing report. |
| Host tests | 31 passed, including Windows DPAPI, admission, transport separation, launcher fallback and access-file import exclusion while a board is busy. |
| ARM simulation | 13 tests of compiled C firmware passed; this is separate from hardware evidence. |
| Initial Ethernet installation | Two matching full backups, erase, complete blank readback, verified bootstrap via SWD, then main application via Ethernet; authenticated version, digest, health and boot confirmation passed. |
| Complete SWD installation | Matching full backups, erase, blank verification, both application slots plus recovery and Stage 0 programmed, complete internal/NOR readback matched the expected layout, and authenticated Ethernet runtime acceptance passed. Total: 1,118 seconds. |
| Physical SWD removal | Operator removed the connection at the board. A distinct version was then installed and confirmed exclusively through Ethernet. |
| Cold boot without SWD | Operator removed independent power for five seconds and restored it. The installed version, digest and confirmed state persisted. |
| Admission and corruption | 16 live checks passed: replay, wrong key, UID/header/generation validation, protected active slot, duplicate chunks, offset/length errors, partial upload and whole-image corruption. Confirmed application remained unchanged. |
| Trial rollback without SWD | Two distinct unconfirmed trial boots were observed. The original confirmed application returned automatically after about 302 seconds; the abandoned generation remained below the preserved generation floor. |
| Incomplete update and power loss | Wrote 262,144 bytes of an inactive candidate without finishing or activating it. After the operator physically cycled power, the previous confirmed application returned healthy through Ethernet alone. |
| Maximum payload and subsequent update | After rollback and the incomplete-transfer power loss, installed a 4,190,208-byte image (the normal firmware plus padding), matched its full digest, booted it and explicitly confirmed generation 4. |

The initial main image was 135,384 bytes. Its network transfer took 1.884 seconds
(71,843 bytes/s); transfer plus boot and confirmation took 12.727 seconds. The
one-time backup/erase/bootstrap workflow took 836 seconds. The subsequent changed
version transferred in 1.868 seconds. These are observed values for this board and
image, not a cable-rate guarantee or a measured comparison against the same SWD
transfer. Flash erase/programming and verification are part of the transfer cost.

The maximum-size image transferred in 53.408 seconds (78,456 bytes/s); the complete
network operation including boot verification took 71.827 seconds. Padding exercises
image storage, transfer, validation and loading at capacity, not four megabytes of
independent application features.

## Published package verification

GitHub Actions run `35862788051` built and published **v0.2.1** from commit
`1e2613149a1faec1e91d73306c378000992bd602`; all host, ARM, source and packaged-CMD
checks passed. The public ZIP was downloaded again, its SHA-256 matched GitHub's
asset digest, its included installer matched source, and its actual CMD passed.

The CI application is 134,824 bytes and recovery is 77,952 bytes. Their sizes
differ from the original local builds because lwIP assertion messages embed
absolute source paths. Rebuilding locally with only those macro paths mapped to
the CI checkout produced byte-for-byte matches for both published binaries.
Stage 0 was already identical. This is a build-path difference, not a firmware
feature change.

The downloaded installer's UI then installed the published main image over
Ethernet: 1.860 seconds for transfer and 13.259 seconds including reboot and
authenticated acceptance. The running digest matched
`4324602cb8996debd3eeb6c6f30e538bbee6068eda85ac16af080351d8a3078d`, the application
was healthy, and generation 2 was confirmed. No SWD tool was used. Physical SWD
removal was proven during the earlier tests; final disconnection after the
separate SWD-route test was not assumed without operator confirmation.

## Remaining deployment qualification

Multiple simultaneous boards, routed/VPN/DHCP deployments, deliberate CPU crashes,
and precisely timed cuts during metadata/configuration/header commits need broader
qualification. Manual power cycling does not prove those precise interruption points.
The release remains a development/manufacturing pilot; see the architecture document
for the security model and the distinction between update authentication and secure boot.

## Repeatable Ethernet tests

Run from the source checkout using the Windows account that enrolled the board.
Replace the UID and IP placeholders with an explicitly selected test board.

```powershell
python tools/test_ota_admission.py --uid <UID> --ip <IP> --image <firmware.bin> --exercise-inactive-slot --output <admission.json>
python tools/test_ota_rollback.py --uid <UID> --ip <IP> --image <distinct-firmware.bin> --version <candidate-version> --exercise-trial-rollback --output <rollback.json>
python tools/test_ota_interruption.py prepare --uid <UID> --ip <IP> --image <firmware.bin> --exercise-inactive-slot --output <interruption.json>
# Physically cycle independent board power with SWD disconnected, then:
python tools/test_ota_interruption.py check --uid <UID> --ip <IP> --power-cycle-confirmed --output <interruption.json>
```

The rollback test deliberately withholds confirmation from a healthy compatible
candidate. It tests the trial timeout and boot policy, not a crashing firmware.
The interruption test writes only a prefix of an inactive candidate and never
activates it. Both tests retain the previous confirmed image.
