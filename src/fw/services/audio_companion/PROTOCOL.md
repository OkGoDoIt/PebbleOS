# Audio Companion Protocol — Version 1

Status: normative. This document is the single source of truth for the wire protocol between
custom Pebble firmware (the Audio Companion GATT service) and the third-party audio companion
app. The firmware tree carries a verbatim copy at
`PebbleOS/src/fw/services/audio_companion/PROTOCOL.md`; the version stamps must match.

Protocol version: **1**
Spec revision: 2026-06-11

## 1. Transport

The watch hosts a GATT server service. The phone app is a GATT client. All stream data flows
watch → phone as notifications; all phone → watch requests are characteristic writes.

Base UUID: `7C2B0000-9E4D-4FC2-A2B3-1D6E8A1C9F50` (third-party-owned; deliberately not the
Pebble base UUID).

| Item | UUID | Properties |
|---|---|---|
| Audio Companion Service | `7C2B0001-9E4D-4FC2-A2B3-1D6E8A1C9F50` | primary service |
| Info characteristic | `7C2B0002-9E4D-4FC2-A2B3-1D6E8A1C9F50` | Read (encrypted link required) |
| Control characteristic | `7C2B0003-9E4D-4FC2-A2B3-1D6E8A1C9F50` | Write (encrypted), Notify |
| Data characteristic | `7C2B0004-9E4D-4FC2-A2B3-1D6E8A1C9F50` | Notify |

- Every write to Control is exactly one control request message.
- Every notification on Control is exactly one control response/event message.
- Every notification on Data is exactly one stream message.
- Messages are never fragmented across writes/notifications. A message must fit in
  (ATT_MTU − 3) bytes; senders size `STREAM_DATA` batches accordingly.

## 2. Encoding Rules

- All multi-byte integers are **little-endian**.
- All structs are packed (no padding).
- Parsers MUST accept messages **longer** than the version-1 size for a known message id and
  ignore the trailing bytes (forward compatibility: future versions append fields only).
- Parsers MUST reject messages **shorter** than the version-1 size for a known message id.
- Unknown message ids MUST be ignored (logged at most).

## 3. Info Characteristic (Read)

A fixed 20-byte snapshot:

| offset | size | field | notes |
|---|---|---|---|
| 0 | 1 | `info_version` | = 1 |
| 1 | 1 | `protocol_min` | lowest protocol version the watch speaks (= 1) |
| 2 | 1 | `protocol_max` | highest protocol version the watch speaks (= 1) |
| 3 | 1 | `service_state` | enum, Section 7 |
| 4 | 1 | `codec_bitmap` | bit0 = Speex wideband |
| 5 | 1 | `flags` | bit0 = a receiver authorization exists, bit1 = enabled pref on, bit2 = consent prompt pending |
| 6 | 2 | `reserved0` | 0 |
| 8 | 4 | `watch_capabilities` | reserved bitfield, 0 |
| 12 | 4 | `fw_version_packed` | `(major << 24) \| (minor << 16) \| patch`, diagnostics only |
| 16 | 4 | `reserved1` | 0 |

## 4. Control Channel

One-byte `msg_id` followed by a packed struct. Phone→watch writes use ids `0x01–0x3F`;
watch→phone control notifications use `0x41–0x7F`. The phone keeps at most one request in
flight; the watch answers every request with a response notification carrying the same
`request_token`.

### 4.1 Phone → Watch (writes)

| id | name | total size (v1) |
|---|---|---|
| `0x01` | `AUTH_REQUEST` | 36 + name_len |
| `0x02` | `AUTH_REVOKE` | 34 |
| `0x03` | `CHECKPOINT` | 26 |
| `0x04` | `PAUSE_REQUEST` | 3 |
| `0x05` | `RESUME_REQUEST` | 2 |
| `0x06` | `RECEIVER_HEALTH` | 8 |

`AUTH_REQUEST` (`0x01`):

| offset | size | field |
|---|---|---|
| 0 | 1 | msg_id = 0x01 |
| 1 | 1 | `proto_version` — highest protocol version the phone speaks |
| 2 | 1 | `request_token` |
| 3 | 32 | `receiver_id` — random bytes generated once per app install |
| 35 | 1 | `name_len` (≤ 24) |
| 36 | name_len | `name` — UTF-8, not NUL-terminated |

`AUTH_REVOKE` (`0x02`): `u8 msg_id`, `u8 request_token`, `u8 receiver_id[32]` (must match the
currently authorized session's receiver id).

`CHECKPOINT` (`0x03`):

| offset | size | field |
|---|---|---|
| 0 | 1 | msg_id = 0x03 |
| 1 | 1 | `request_token` |
| 2 | 4 | `stream_id` |
| 6 | 4 | `highest_contiguous_sequence_persisted` |
| 10 | 8 | `persisted_sample_index` |
| 18 | 4 | `receiver_flags` — bit0 = LOW_STORAGE, bit1 = PAUSE_REQUESTED |
| 22 | 4 | `free_storage_hint_kb` |

`PAUSE_REQUEST` (`0x04`): `u8 msg_id`, `u8 request_token`, `u8 reason`
(1 low-storage, 2 user, 3 policy).

`RESUME_REQUEST` (`0x05`): `u8 msg_id`, `u8 request_token`.

`RECEIVER_HEALTH` (`0x06`): `u8 msg_id`, `u8 request_token`, `u8 battery_pct`,
`u8 app_state` (1 foreground, 2 background, 3 restored), `u32 queue_depth_frames`.

### 4.2 Watch → Phone (control notifications)

| id | name | total size (v1) |
|---|---|---|
| `0x41` | `AUTH_RESULT` | 4 |
| `0x42` | `REVOKED` | 2 |
| `0x43` | `ACK` | 3 |
| `0x44` | `STATE_CHANGED` | 2 |
| `0x45` | `ERROR` | 6 |

`AUTH_RESULT` (`0x41`): `u8 msg_id`, `u8 request_token`, `u8 status`, `u8 granted_proto_version`.
Status: 0 ok, 1 pending-user-consent, 2 denied-mismatch, 3 denied-disabled, 4 invalid.
`granted_proto_version = min(phone proto_version, watch protocol_max)`; meaningful only for
status 0.

When an `AUTH_REQUEST` resolves asynchronously (status 1 was returned while the consent prompt
is shown), the watch later pushes a **second** `AUTH_RESULT` with the same `request_token` and
the final status (0 on accept; 4 on decline or timeout).

`REVOKED` (`0x42`): `u8 msg_id`, `u8 reason` (1 user-on-watch, 2 app-requested, 3 replaced).
Unsolicited (no request token).

`ACK` (`0x43`): `u8 msg_id`, `u8 request_token`, `u8 status` (0 ok, 1 rejected, 2 bad-state).
Response to `CHECKPOINT`, `PAUSE_REQUEST`, `RESUME_REQUEST`, and `RECEIVER_HEALTH`.

`STATE_CHANGED` (`0x44`): `u8 msg_id`, `u8 service_state` (Section 7). Pushed on every service
state transition while a control subscription exists.

`ERROR` (`0x45`): `u8 msg_id`, `u8 error_code`, `u32 detail`. Error codes: 1 malformed message,
2 unauthorized, 3 internal, 4 unsupported version.

## 5. Data Channel

One-byte `msg_id` (`0x80–0x9F`) plus packed struct per notification.

| id | name | total size (v1) |
|---|---|---|
| `0x80` | `STREAM_START` | 40 |
| `0x81` | `STREAM_DATA` | 20 + frames |
| `0x82` | `STREAM_GAP` | 26 |
| `0x83` | `STREAM_STOP` | 22 |

`STREAM_START` (`0x80`):

| offset | size | field |
|---|---|---|
| 0 | 1 | msg_id = 0x80 |
| 1 | 1 | `protocol_version` (granted version) |
| 2 | 4 | `stream_id` — watch-random, nonzero, new per (re)start from idle |
| 6 | 1 | `codec_id` (Section 7) |
| 7 | 1 | `channels` (1) |
| 8 | 2 | `frame_samples` (320) |
| 10 | 4 | `sample_rate_hz` (16000) |
| 14 | 4 | `bit_rate_bps` (9800) |
| 18 | 2 | `frame_duration_ms` (20) |
| 20 | 8 | `start_time_ms` — watch wall clock, UTC ms |
| 28 | 8 | `start_monotonic_ms` — watch monotonic ms |
| 36 | 4 | `flags` (0) |

`STREAM_DATA` (`0x81`) header (20 bytes), followed by `frame_count` frame entries:

| offset | size | field |
|---|---|---|
| 0 | 1 | msg_id = 0x81 |
| 1 | 4 | `stream_id` |
| 5 | 4 | `first_sequence` |
| 9 | 8 | `first_sample_index` |
| 17 | 1 | `frame_count` (1–32) |
| 18 | 2 | `flags` (0) |

Each frame entry: `u16 length` + `u8 payload[length]`. Frames are consecutive: frame *i* has
sequence `first_sequence + i` and sample index `first_sample_index + i * frame_samples`.
`length` ≤ 200 (`MAX_ENCODED_FRAME_BYTES`). Batching: the watch packs as many frames as fit in
(ATT_MTU − 3 − 20), capped at 32.

`STREAM_GAP` (`0x82`):

| offset | size | field |
|---|---|---|
| 0 | 1 | msg_id = 0x82 |
| 1 | 4 | `stream_id` |
| 5 | 4 | `first_missing_sequence` |
| 9 | 4 | `missing_frame_count` — 0 = unknown / elapsed-time-only gap |
| 13 | 8 | `first_missing_sample_index` |
| 21 | 1 | `reason` (Section 7) |
| 22 | 4 | `watch_drop_counter` — cumulative overflow-dropped frames |

`STREAM_STOP` (`0x83`):

| offset | size | field |
|---|---|---|
| 0 | 1 | msg_id = 0x83 |
| 1 | 4 | `stream_id` |
| 5 | 1 | `reason` (Section 7) |
| 6 | 4 | `final_sequence` — last sequence assigned (0 if none) |
| 10 | 8 | `final_sample_index` — total samples spanned |
| 18 | 4 | `counters_crc_or_zero` (0 in v1) |

Sequence semantics: `sequence` increments per encoded frame within a stream, starting at 0.
`sample_index` is the cumulative count of 16 kHz samples since stream start, advancing by
`frame_samples` per delivered frame and by the estimated missed sample count across gaps.

## 6. Authorization And Session Flow

1. Phone subscribes to Control, reads Info.
2. Phone writes `AUTH_REQUEST{receiver_id, name}`. `receiver_id` is 32 random bytes generated
   once per app install, stored in platform-secure storage.
3. Watch behavior:
   - Feature pref disabled → `AUTH_RESULT(denied-disabled)`.
   - No stored receiver → consent prompt on watch → `AUTH_RESULT(pending-user-consent)`;
     on user accept the watch stores SHA-256(receiver_id) + name + timestamp and pushes
     `AUTH_RESULT(ok)`; on decline/timeout (60 s) nothing is stored and a later
     `AUTH_REQUEST` re-prompts.
   - Stored hash matches SHA-256(receiver_id) → `AUTH_RESULT(ok)` immediately.
   - Stored hash differs → `AUTH_RESULT(denied-mismatch)`. Fail closed; the user must revoke
     on the watch before a different install can bind.
4. After `AUTH_RESULT(ok)` the session is authorized for the lifetime of the BLE connection.
   The phone subscribes to Data; the watch streams per policy.
5. Any disconnect ends the session; reconnect repeats the handshake (silent when the hash
   matches).
6. Revocation: watch Settings ("Forget Receiver") or `AUTH_REVOKE` → stored identity wiped,
   `REVOKED` pushed, streaming stops, service returns to idle.

Invariant: the watch never sends `STREAM_DATA` before `AUTH_RESULT(ok)` on the current
connection with Data subscribed.

## 7. Enums And Constants

Service states (Info `service_state`, `STATE_CHANGED`):

| value | state |
|---|---|
| 0 | disabled |
| 1 | idle (enabled, waiting for receiver) |
| 2 | authorized-idle |
| 3 | streaming |
| 4 | paused-conflict (mic owned by dictation) |
| 5 | paused-policy (receiver requested pause / low storage) |
| 6 | paused-low-battery |
| 7 | error |

Codec ids: `0x01` Speex wideband, `0x02` PCM16 debug, `0x03` Opus (reserved), `0x04` LC3
(reserved).

Gap reasons: `0x01` spool overflow, `0x02` mic conflict, `0x03` user disabled,
`0x04` low battery, `0x05` codec error, `0x06` transport reset.

Stop reasons: `0x01` user disabled, `0x02` policy, `0x03` error, `0x04` shutdown.

Constants:

| name | value |
|---|---|
| `MAX_ENCODED_FRAME_BYTES` | 200 |
| `DEFAULT_FRAME_SAMPLES` | 320 |
| `DEFAULT_SAMPLE_RATE_HZ` | 16000 |
| `DEFAULT_FRAME_DURATION_MS` | 20 |
| `DEFAULT_BIT_RATE_BPS` | 9800 |
| `MAX_FRAMES_PER_DATA_MSG` | 32 |
| `MAX_RECEIVER_NAME_BYTES` | 24 |
| `CONSENT_TIMEOUT_SECONDS` | 60 |

## 8. Version Negotiation

`AUTH_REQUEST.proto_version` is the highest version the phone speaks;
`AUTH_RESULT.granted_proto_version = min(phone, watch protocol_max)`. All subsequent messages
use the granted version. Future versions extend structs only by appending fields
(size-discriminated parsing) or by adding new message ids.

## 9. Golden Fixtures

`spec/fixtures/` contains, per message and notable edge case, a `<name>.bin` (exact wire
bytes) and `<name>.json` (field values + description). Both are generated by
`tools/gen_fixtures.py` — edit that script, never the artifacts. `tools/fixtures_to_c.py`
emits a C header consumed by the firmware test
`tests/fw/services/test_audio_companion_protocol.c`; KMP tests read the files directly.
Any fixture change requires a protocol version decision.
