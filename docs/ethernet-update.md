# Independent Ethernet installation and updates (protocol 2)

## Boot and memory layout

| Region | Purpose | Written during an ordinary firmware update? |
| --- | --- | --- |
| Internal Flash, 128 KiB | Small Stage 0; clocks, memory checks and image selection | No |
| NOR `0x010000..0x0DFFFF` | Independent recovery image | No |
| NOR `0x0E0000..0x0E0FFF` | UID-bound 256-bit update credential | No |
| NOR `0x0E1000..0x0E4FFF`, `0x0E5000..0x0E8FFF` | Redundant confirmed settings/protocol records | Configuration API only |
| NOR `0x0F0000`, `0x0F1000` | Alternating boot metadata sectors | Yes, atomic commit |
| NOR `0x100000..0x4FFFFF`, `0x500000..0x8FFFFF` | Application A/B, 4092 KiB usable payload each | Inactive slot only |

On ordinary resets Stage 0 loads recovery. Recovery restores the confirmed network configuration and opens a rescue window, or waits indefinitely if no valid application exists. A one-use RTC backup marker allows a software reset to select the application. Stage 0 consumes this marker and only accepts it for a software reset; watchdog/power resets always reopen recovery. The marker never grants permission to boot an unvalidated image.

Trial attempt counts are persisted before execution. After two failed attempts or an invalid trial, selection clears the abandoned trial, retains its generation floor and selects only a valid confirmed image. It never promotes an arbitrary unconfirmed slot. With no confirmed image it returns to recovery. A valid recovery region is therefore necessary for autonomous recovery; physical damage to it/internal Flash or memory hardware remains a service case.

The application does not auto-confirm just because memory tests pass. It must report ready health, correct digest, UID and version to an authenticated client. The client explicitly confirms the trial. The two-minute trial timeout resets an unconfirmed application. Recovery/application writes cannot address Stage 0, factory or the provisioned key through the update API.

## Provisioning and detach

The initial SWD package consists of Stage 0 plus recovery and per-device metadata/credentials. It deliberately leaves A/B erased for the Ethernet installation route. Host dependencies and complete release files are available before erasing. The final OpenOCD script verifies the written regions, releases reset, clears debugger watchdog freeze/core debug and shuts down. A checked process exit is the software detach boundary; no fuse or permanent SWD lock is changed. Initial backup/erase/blank checks are separate, earlier SWD operations.

## Wire protocol

Use `installer/ethernet.py`; `tools/ota_update.py` is its CLI wrapper. There is no fallback to the old unauthenticated protocol.

The update API uses its per-device key independently of the legacy source-IP allow-list, so DHCP clients and routed/VPN management hosts can update a board. Existing non-update endpoints retain their source-IP restrictions. UDP port 37020 accepts `G100_UPDATE_DISCOVER_V2` padded with zero bytes to exactly 256 bytes. Replies contain only protocol, UID and firmware hints, never exceed the request size, and are limited to five per second per board. Legacy discovery policies remain unchanged. The installer verifies hints with authenticated HTTP before writing.

`GET /api/v1/firmware/status` returns an unauthenticated discovery/challenge document. It contains protocol version 2, UID, role, boot nonce, next sequence, boot/upload state, running image hash and maximum block size. Treat these as hints until an authenticated POST proves the peer.

All POST bodies use `application/octet-stream`:

`uint32_le sequence | 32-byte HMAC-SHA256 | binary payload`

Request MAC input is `boot_nonce[32] || sequence_le[4] || full_request_path_ASCII || NUL || payload`. Keys are unique to a board and provisioned by SWD. A nonce comes from the hardware RNG; missing key/RNG fails closed. Accepted request sequence numbers cannot be replayed. Successful authentication consumes the sequence even if semantic validation rejects the operation.

An authenticated response includes `X-G100-MAC`, the lowercase HMAC hex over `boot_nonce || request_sequence_le || "response" || NUL || raw_response_body`. The client verifies it before believing success or errors. This authenticates the peer and result; it does not encrypt the firmware or metadata.

| POST suffix under `/api/v1/firmware/` | Payload | Result |
| --- | --- | --- |
| `status` | Empty | Authenticated state and identity |
| `begin?uid=<24 uppercase hex>` | 128-byte packed image header | Select inactive slot and invalidate its old header; repair lost metadata only in recovery |
| `chunk?offset=<decimal>` | 4..16384 binary bytes, aligned to 4 bytes | Sequential NOR write and readback; identical acknowledged ranges may be retried |
| `finish` | Empty | Full NOR CRC/SHA check, vector validation and header commit |
| `activate` | Empty | Atomic trial metadata and delayed software reset |
| `confirm` | Running payload SHA-256, 32 bytes | Accept only the healthy running image matching the digest |
| `abort` | Empty | Forget an incomplete upload; confirmed image remains protected |
| `recovery` | Empty | Reboot into independent recovery |

The SHA-256 in an authenticated begin header binds the entire candidate to the authorized sender. Image generation must exceed the preserved floor; this prevents replay of an old transaction, but is not a vendor-version anti-downgrade policy. A trusted operator can intentionally select an older compatible build with a new transaction generation. Stage 0 still performs CRC integrity validation, not cryptographic secure boot.

Chunk retries use a fresh authentication sequence after a lost response. Byte comparisons make repeated chunks idempotent. Partial/corrupt payloads never receive a valid image header. Activation checks metadata has not changed since begin. Writes to the previous confirmed slot are forbidden. Lost responses around reset are resolved by authenticating the running image after reboot, never by assuming activation succeeded.

## Confirmed configuration

Settings and protocol instances are committed to an alternate NOR record with CRC and magic written last. Failed writes leave the previous record readable. Network settings are saved only after the existing reconnect-and-confirm transaction succeeds; an unconfirmed network change still rolls back. Recovery and the main application read the same confirmed management address, hostname and DNS configuration. Future configuration formats require explicit migration compatible with rollback. Other provisioning credentials/TLS state retain their existing volatile policy.

## Automated verification

- `python -m unittest discover -s installer/tests -v`: host admission, offline manifest verification, initial layouts, DPAPI, exclusive access and no-SWD-after-bootstrap boundary.
- `python tools/test_firmware_arm.py`: builds and executes the actual C updater, HMAC, boot selection and configuration store on ARM emulation with a simulated NOR boundary. Tests cover blank installation, second update, authentication/replay, corruption, retries, rollback followed by another update, lost metadata, interrupted writes and saved configuration.
- `python tools/check_public.py` and `node --check installer/web/app.js`.
- Build `shadow`, `recovery` and Stage 0. ARM emulation is not PHY/NOR electrical testing.

## Required physical acceptance

This is a reference checklist for future qualification, not a pending task list.
The current milestone is complete within the scope recorded below. In particular,
the operator declined a further SWD/Ethernet speed comparison; see the
[continuation handoff](continuation.md) before starting additional hardware work.

Use an expendable board and preserve its verified backup. See the [qualification record](qualification.md) for completed checks and remaining hardware coverage.

1. Install the prepared bootstrap using the new installer; verify no A/B payload is written through SWD on this route.
2. Independently power the board and fully remove SWD. Install the first full application through Ethernet, then verify UID, version, digest and confirmed boot.
3. Power-cycle and verify the application and confirmed configuration persist. Install a visibly changed second version using Ethernet only.
4. Interrupt a transfer and retry; corrupt a candidate; verify the previous app/factory stays usable.
5. Boot a deliberately unhealthy trial; verify rollback and then a successful later Ethernet update.
6. Cut power during payload, image-header, metadata and configuration commits; restore power and recover through Ethernet.
7. Verify the recovery window after a crashing confirmed app and with both app slots invalid.
8. Qualify DHCP/static/routed access, several simultaneous boards, near-capacity images and throughput against an identical SWD image. Do not infer a cable-rate guarantee from emulator results.

References for the peripheral implementation: [STM32H750 reference manual RM0433](https://www.st.com/resource/en/reference_manual/rm0433-stm32h742-stm32h743753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf) and the pinned Mbed TLS 3.6.5 headers shipped with this repository. The former covers backup registers, reset causes, RNG and debug freeze; the source build uses its pinned crypto API rather than an unpinned latest API.
