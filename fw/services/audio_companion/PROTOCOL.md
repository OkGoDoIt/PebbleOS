# Audio Companion Protocol — Version 1

Status: normative. This document is the single source of truth for the wire protocol between
custom Pebble firmware (the Audio Companion GATT service) and the third-party audio companion
app. The firmware tree carries a verbatim copy at
`PebbleOS/fw/services/audio_companion/PROTOCOL.md`; the version stamps must match.

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
| 5 | 1 | `flags` | bit0 = a receiver authorization exists, bit1 = enabled pref on, bit2 = consent prompt pending, bit3 = `send_backpressure_events` is meaningful |
| 6 | 2 | `reserved0` | 0 |
| 8 | 4 | `watch_capabilities` | bit0 Muse setup relay (4.3), bit1 push-to-talk (4.4), bit2 watch actions (4.5), bit3 answer restart (4.4), bit4 follow-up (4.4), bit5 conversation (4.4), bit6 durable moments (4.6); other bits 0 |
| 12 | 4 | `fw_version_packed` | `(major << 24) \| (minor << 16) \| patch`, diagnostics only |
| 16 | 4 | `send_backpressure_events` | notifications the transport refused since service init; 0 unless `flags` bit3 is set |

`send_backpressure_events` was `reserved1`, so the layout, the 20-byte size and `info_version`
are unchanged and a receiver that ignores the field is unaffected. It exists so the phone can
tell two failure modes apart that otherwise look identical from the outside: **airtime loss**
(this counter climbing while audio goes missing — the radio could not absorb the stream, and the
answer is a better link or less contention) and **credit starvation** (this counter flat while
audio still goes missing — the receiver stopped checkpointing, so the spool never freed and the
fix is on the phone). Because firmware that predates the field leaves those bytes zero, a
receiver must check `flags` bit3 before trusting a zero.

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
| `0x07` | `ENABLE_REQUEST` | 2 |
| `0x08` | `PHONE_MEDIA_STATE` | 3 |
| `0x09` | `MUSE_SETUP_OPEN` | 11 |
| `0x0A` | `MUSE_SETUP_CLOSE` | 3 |
| `0x0B` | `MUSE_SETUP_TX` | 3 + length |
| `0x0C` | `MUSE_SETUP_DISCONNECT` | 4 |
| `0x0D` | `ASSISTANT_UPDATE` | 6 + text |
| `0x0E` | `WATCH_ACTION` | 5 + payload |
| `0x0F` | `MOMENT_SAVED` | 11 |

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

The watch runs a **15-second receiver-liveness watchdog** while streaming: any successfully
parsed control write refreshes it, so a receiver with nothing else to send should write
`RECEIVER_HEALTH` roughly every 5 seconds as a keepalive. On expiry the watch presumes the
receiver gone, stops capture (recording a `TransportReset` gap), and drops out of Streaming —
the stream itself stays active. The next control write revives it and triggers a RESUME
re-announcement with a spool rewind, so a receiver that under-sends keepalives sees periodic
capture pauses and RESUME cycles.

`ENABLE_REQUEST` (`0x07`): `u8 msg_id`, `u8 request_token`. When Background Audio is off, this
asks the watch to show an on-watch approval prompt. The watch replies with `ACK(ok)` only after the
user accepts and the setting has been persisted/enabled; decline, timeout, no UI handler, or another
enable prompt already in flight return `ACK(rejected)`/`ACK(bad-state)`. If Background Audio is
already on, the watch may return `ACK(ok)` immediately.

### 4.2 Watch → Phone (control notifications)

| id | name | total size (v1) |
|---|---|---|
| `0x41` | `AUTH_RESULT` | 4 |
| `0x42` | `REVOKED` | 2 |
| `0x43` | `ACK` | 3 |
| `0x44` | `STATE_CHANGED` | 2 |
| `0x45` | `ERROR` | 6 |
| `0x46` | `PHONE_MEDIA_QUERY` | 1 |
| `0x47` | `MUSE_SETUP_RX` | 4 + length |
| `0x48` | `MUSE_SETUP_EVENT` | 7 |
| `0x49` | `PTT_EVENT` | 8 |
| `0x4A` | `WATCH_ACTION_RESULT` | 4 |
| `0x4B` | `MOMENT_MARK` | 24 |

`AUTH_RESULT` (`0x41`): `u8 msg_id`, `u8 request_token`, `u8 status`, `u8 granted_proto_version`.
Status: 0 ok, 1 pending-user-consent, 2 denied-mismatch, 3 denied-disabled, 4 invalid.
`granted_proto_version = min(phone proto_version, watch protocol_max)`; meaningful only for
status 0.

When an `AUTH_REQUEST` resolves asynchronously (status 1 was returned while the consent prompt
is shown), the watch later pushes a **second** `AUTH_RESULT` with the same `request_token` and
the final status (0 on accept; 4 on decline or timeout).

`REVOKED` (`0x42`): `u8 msg_id`, `u8 reason` (1 user-on-watch, 2 app-requested, 3 replaced).
Unsolicited (no request token).

`ACK` (`0x43`): `u8 msg_id`, `u8 request_token`, `u8 status` (0 ok, 1 rejected, 2 bad-state,
3 busy — the watch could not send right now, try again shortly). Response to `CHECKPOINT`,
`PAUSE_REQUEST`, `RESUME_REQUEST`, `RECEIVER_HEALTH`, `ENABLE_REQUEST`, `PHONE_MEDIA_STATE`, and
the `MUSE_SETUP_*` requests.

`STATE_CHANGED` (`0x44`): `u8 msg_id`, `u8 service_state` (Section 7). Pushed on every service
state transition while a control subscription exists.

`ERROR` (`0x45`): `u8 msg_id`, `u8 error_code`, `u32 detail`. Error codes: 1 malformed message,
2 unauthorized, 3 internal, 4 unsupported version.

`PHONE_MEDIA_QUERY` (`0x46`): `u8 msg_id`. Unsolicited. Asks the receiver whether the phone is
actually playing audio. The phone's media service (AMS on iOS) reports what a player publishes, and
a player can publish "playing" while paused, so the watch asks the phone's audio system before
showing music as playing. The receiver answers with `PHONE_MEDIA_STATE`; a receiver that does not
know this id ignores it and the watch falls back to the media service alone.

`PHONE_MEDIA_STATE` (`0x08`): `u8 msg_id`, `u8 request_token`, `u8 flags`. Bit0 `CHECKED`: the
phone could check (when clear, the other bits mean nothing); bit1 `OTHER_AUDIO`: another app is
playing audio (iOS `AVAudioSession.isOtherAudioPlaying`); bit2 `PRIMARY_AUDIO`: that audio is
non-mixable primary audio (iOS `secondaryAudioShouldBeSilencedHint`). Sent in answer to
`PHONE_MEDIA_QUERY`, or unprompted when the receiver notices a change. The watch replies with
`ACK`.

### 4.3 Muse gadget setup relay

Meta's Muse app pairs a "gadget" over BLE (Muse Gadget SDK): the gadget advertises as
`MuseGadgetXXXXXX` with setup service `7fdd3d1c-38ea-46cf-8b46-314ecf5f240c`, the app writes
JSON records to its RX characteristic (`4d593029-…`) and reads replies as notifications on its
TX characteristic (`d75dc4ca-…`). These messages let the receiver app be that gadget: for a short
**window** the watch hosts the setup service on a second, private connection and relays every
characteristic value between the Muse app and the receiver, which runs the setup protocol and
holds every key. The watch never parses the setup records. The watch advertises the window
`watch_capabilities` bit0 (`MUSE_SETUP`); a watch without it ignores these ids.

The window needs the phone link up, and the user confirms it on the watch first. All of these
need an authorized receiver (else `ERROR(unauthorized)`).

`MUSE_SETUP_OPEN` (`0x09`): `u8 msg_id`, `u8 request_token`, `u16 window_secs` (clamped to
30–600), `char suffix[6]` (the six uppercase hex digits after `MuseGadget`, the gadget identity's
suffix), `u8 flags` (0). `ACK(ok)` means the watch is asking the user; the outcome follows as
`MUSE_SETUP_EVENT` Opened or Closed. `ACK(bad-state)`: a window is already pending or open.
`ACK(rejected)`: bad suffix, or the watch cannot ask the user.

`MUSE_SETUP_CLOSE` (`0x0A`): `u8 msg_id`, `u8 request_token`, `u8 reason` (1 paired, 2 cancelled,
3 failed). Ends the window; the watch shows the matching ending. `ACK(bad-state)` if none is open.

`MUSE_SETUP_TX` (`0x0B`): `u8 msg_id`, `u8 request_token`, `u8 length` (1–240), `u8 data[length]`.
One value to notify on the setup TX characteristic, verbatim; the receiver does the Muse app's
`0xFE` chunk framing itself. `ACK(ok)` once handed to the transport; `ACK(rejected)` when the Muse
app is not connected and subscribed; `ACK(busy)` when the transport refused it (retry);
`ACK(bad-state)` when no window is open.

`MUSE_SETUP_DISCONNECT` (`0x0C`): `u8 msg_id`, `u8 request_token`, `u16 delay_ms` (advisory).
Ends the Muse app's link; the window stays open so it can connect again. `ACK(bad-state)` if no
link.

`MUSE_SETUP_RX` (`0x47`): `u8 msg_id`, `u8 seq`, `u8 flags`, `u8 length`, `u8 data[length]`.
Unsolicited: one value the Muse app wrote to the setup RX characteristic. A value longer than
one notification carries is split across notifications that share `seq`, with `flags` bit0
`MORE` set on all but the last; the receiver concatenates them in order. `seq` increments per
value, so a gap means a value was lost (see `RxOverflow`).

`MUSE_SETUP_EVENT` (`0x48`): `u8 msg_id`, `u8 event`, `u8 detail`, `u16 mtu`, `u16 seconds_left`.
Unsolicited. `mtu` is the Muse link's ATT MTU (0 when none) and `seconds_left` the window's
remaining time. Events: 1 `Opened` (advertising); 2 `ClientConnected`; 3 `Subscribed` (detail 1
subscribed to TX, 0 unsubscribed); 4 `ClientDisconnected` (detail = HCI reason; the window stays
open while time remains); 5 `Closed` (detail: 1 app-paired, 2 app-cancelled, 3 app-failed,
4 timeout, 5 cancelled on the watch, 6 declined on the watch, 7 phone link lost, 8 could not
start, 9 receiver gone); 6 `RxOverflow` (detail = values dropped since the last report);
7 `Mtu` (the link's MTU changed).

Order of events on a typical pairing: `Opened` → `ClientConnected` → `Mtu` → `Subscribed(1)` →
`RX`/`TX` exchange → the receiver sends `CLOSE(paired)` → `Closed(1)`.

### 4.4 Push-to-talk

The wearer holds a button, asks the receiver's assistant a question, and reads the answer on the
watch. The watch sends exactly the question's audio, as it is captured, apart from the recording
stream: it never goes to the spool, is never silence-skipped, and needs no stream to be running.
When the recording stream is also running, the same mic frames go into it too. Advertised by
`watch_capabilities` bit1 (`PUSH_TO_TALK`); needs an authorized receiver subscribed to Control
and Data.

The mic is on for a question only while the button is held. The watch may start it at the
press, before the hold is confirmed, so the first words aren't clipped; if the button is released
before the hold is confirmed, that audio is discarded on the watch and nothing is sent.

`PTT_EVENT` (`0x49`, watch → phone): `u8 msg_id`, `u8 turn` (1–255, new for each question, never
0), `u8 event`, `u8 detail`, `u16 frame_count`, `u16 max_seconds`.
- `1 Start`: the hold is confirmed. `detail` is the trigger (1 Quick Launch hold, 2 hold inside
  the watch's assistant screen, 3 a follow-up); `max_seconds` is the longest question the watch
  records (15). Frames from the press onward follow as `PTT_AUDIO`.
- `2 End`: the question is complete. `detail` 1 released, 2 reached `max_seconds`;
  `frame_count` is the number of frames sent (`0 .. frame_count-1`).
- `3 Cancel`: no question. `detail` 1 too short (under 0.3 s), 2 cancelled, 3 receiver
  unavailable, 4 mic unavailable. Discard any frames of this turn.
- `4 Pick`: the wearer picked option `detail` (0–5) of turn `turn`'s answer (`Option` below),
  on the assistant screen or from a notification, maybe long after the answer. `frame_count` and
  `max_seconds` are 0. The receiver sends that option to the assistant as the wearer's reply
  and answers on the same turn, as for a question: `Heard` (the option's words), then the
  answer. A receiver that no longer knows the turn's options answers `Error` 3. Needs bit5.

`PTT_AUDIO` (`0x84`, Data, watch → phone): `u8 msg_id`, `u8 turn`, `u16 first_frame`,
`u8 frame_count` (1–32), then `frame_count` entries of `u16 length`, `u8 payload[length]`, as in
`STREAM_DATA` (Speex wideband frames, 20 ms each, the codec of `STREAM_START`). Frames are numbered from 0 at the
press; a missing range is lost audio. Sent as the frames are encoded, at most one mic buffer
(about 40 ms) behind.

`ASSISTANT_UPDATE` (`0x0D`, phone → watch): `u8 msg_id`, `u8 request_token`, `u8 turn`,
`u8 kind`, `u8 detail`, `u8 length` (0–180), `u8 text[length]` (UTF-8, cut on a character
boundary). The watch shows it for that turn and ACKs; an update for a turn the watch no longer
shows is ACKed and ignored.
- `1 Heard`: what the assistant understood the question to be; replaces any earlier one. After
  `Done` it starts a follow-up on the same turn: the answer is cleared and the watch waits for
  the next one (the wearer picked an option the assistant offered, Section 4.5 `Choose`). Needs
  `watch_capabilities` bit4; a receiver sends it after `Done` only to a watch that sets it.
- `2 Status`: what the assistant is doing ("Searching the web"); replaces any earlier one.
- `3 Answer`: more of the answer, appended. Plain text; line breaks allowed.
- `4 Done`: the answer is complete.
- `5 Error`: the turn failed; `detail` 1 assistant not set up, 2 assistant unreachable, 3 no
  answer, 4 didn't catch that, 5 busy with another question; text optional.
- `6 AnswerRestart`: the answer so far is replaced by this text, and `Answer` appends to it from
  here. Sent when the assistant's finished answer differs from the words it streamed. Needs
  `watch_capabilities` bit3; a receiver sends it only to a watch that sets it.
- `7 Begin`: the receiver begins a turn of its own, for a message the assistant sent by itself
  (`detail` 0: a scheduled result, an update, a worker's report) or for an answer that came after
  the receiver stopped waiting (`detail` 1: `text` is the question it answers). `turn` is the
  receiver's choice; the watch's next question takes the turn after it. The watch starts a new
  exchange, shown as answering; `Answer`, `AnswerRestart`, `Option` and `Done` for that turn
  follow. While one of the wearer's questions is being asked or waits for its answer the watch
  ACKs `busy`, and the receiver sends it again once that question has ended. Needs bit5.
- `8 Option`: one thing the wearer can pick to reply: `detail` its index (0–5, in order), `text`
  its words (1–48 bytes). Sent after the answer's text, before `Done`. `AnswerRestart`, a
  follow-up `Heard` and `Begin` clear the options. The watch shows them under the answer and on
  its notification; a pick comes back as `PTT_EVENT Pick`. Needs bit5; to a watch without it,
  the receiver offers a choice with `WATCH_ACTION Choose` (Section 4.5) instead.

`Error` `detail` 6 (still working): the receiver stopped waiting while the assistant still works
on it. Its answer comes later as a `Begin` with `detail` 1. Needs bit5.

With bit5 (conversation) the exchange outlives the assistant screen. Closing the screen
abandons only a question whose button is still held; one already released keeps going, and
updates for it keep applying. An answer (`Done`) or a failure (`Error`, except 6 and the
watch's own phone- and mic-unavailable errors) that arrives while the screen is closed becomes
an ordinary watch notification, so the wearer's notification settings apply to it. Its actions
are the answer's options, a reply (which opens the screen on the conversation) and Dismiss.
Opening the screen takes back the notifications posted while it was closed. A screen that
opens shows the exchange as it is. The watch keeps the link quick for 90 s of quiet after a
question and waits up to 15 minutes for word of it; ending it before that is the receiver's
job.

Typical order: `PTT_EVENT Start` → `PTT_AUDIO`… → `PTT_EVENT End` → `Status`… → `Heard` →
`Answer`… → `Option`… → `Done`; later, maybe, `PTT_EVENT Pick` → `Heard` → `Answer`… → `Done`.
A message of the assistant's own: `Begin` → `Answer`… → `Option`… → `Done`.

### 4.5 Watch actions

During a question the receiver's assistant may ask the watch to do something. Advertised by
`watch_capabilities` bit2 (`WATCH_ACTIONS`); needs an authorized receiver.

`WATCH_ACTION` (`0x0E`, phone → watch): `u8 msg_id`, `u8 request_token`, `u8 action_id`
(nonzero, chosen by the phone), `u8 flags` (bit0 `MORE`), `u8 length` (1–180),
`u8 payload[length]`. A payload longer than one write is split across writes that share
`action_id`, `MORE` set on all but the last; the joined payload is at most 512 bytes. Each
write is ACKed: `ok` when taken, `rejected` when malformed or unknown, `busy` when a choice is
already showing.

Joined payload: `u8 kind`, then (strings are `u8 length` + UTF-8):
- `1 Timer`: `u32 seconds` (1–86400), `str label` (≤ 48). A countdown on the watch; when it
  ends the watch vibrates and shows the label.
- `2 Alarm`: `u8 hour` (0–23), `u8 minute` (0–59), `str label` (≤ 48). A one-off alarm in the
  watch's alarms, at the next such time.
- `3 Choose`: `u16 timeout_s` (10–300), `str question` (≤ 120), `u8 count` (2–6), then
  `count` × `str option` (≤ 48). The watch vibrates and asks; the wearer picks one or backs out.
- `4 Music`: `u8 command` — 1 play, 2 pause, 3 play/pause, 4 next, 5 previous, 6 volume up,
  7 volume down, 8 now playing (changes nothing). Sent to the phone's player over the watch's own
  media link. Result text: what is playing, `title\nartist`, when known. Failed: no player.
- `5 QuietTime`: `u8 on`, `u16 minutes` (0: until turned off). With minutes, the watch turns it
  back off itself.
- `6 LaunchApp`: `str name` (≤ 48). Opens the launcher app whose name matches best (exact, then
  starts with, then contains; case-insensitive; never a watchface or a hidden app). Result text:
  the app's name. Failed: nothing matched.
- `7 FindWatch`: `u8 seconds` (1–30, 0 means 10). Buzzes and lights up.
- `8 Health`: `u8 metric` — 1 steps, 2 distance (m), 3 active seconds, 4 active kcal, 5 sleep
  seconds, 6 heart rate, 7 heart rate by day — and `u8 days` (1–7). Result text: the daily values
  comma-separated, today first, `-1` for a day without data; for heart rate, `bpm,age_seconds` of
  the latest reading; for heart rate by day, each day's `average/lowest/highest` bpm from the
  watch's minute history. Failed: no activity data; for heart rate, the text `off` when
  background heart rate is turned off on the watch. Heart rate by day is read a slice at a time
  and can take a few seconds.
- `9 Notice`: `str text` (≤ 120). A short message from the receiver itself (not the assistant),
  shown on the watch until dismissed or for 15 s.
- `10 Alarms`: `u8 operation` (0 list, 1 create, 2 update, 3 delete), `u8 id` (0–9),
  `u8 hour`, `u8 minute`, `u8 smart`, `u8 enabled`. Hour/minute use 0–23 / 0–59;
  smart/enabled use 0/1. A field of 255 preserves it on update. Create requires time,
  smart and enabled=1, creates a one-time alarm and returns its id in result `value`.
  Update/delete require an existing id; update preserves recurrence, sound and vibration.
  List ignores fields and returns all alarms, one fixed-width `iHHMMsekDD\n` record each:
  id, hour, minute, smart, enabled, native kind (0 daily, 1 weekends, 2 weekdays,
  3 once, 4 custom), two hex digits of scheduled days (Sunday bit0). Ten records fit
  in 110 bytes. Empty text means no alarms. Smart wake uses the native fixed 30-minute
  window BEFORE the given time. Labels are not stored. Older firmware returns Unsupported;
  the phone may fall back to kind 2 only for regular one-time creation.


- `11 Timers` (legacy) and `12 NativeTimers`: `u8 operation` (0 list, 1 create, 2 get,
  3 pause, 4 resume, 5 update, 6 restart, 7 cancel), `u32 id`, `u32 seconds`,
  `u8 label_present`, `str label`. List/create require id 0; other operations require a
  nonzero id. Create seconds are 1–86400; update seconds 0 preserves the duration. Update
  needs seconds or label_present; other operations cannot carry a duration or label.
  A nonempty label is rejected because Core Devices Timer does not support labels.
  New receivers MUST use kind 12 and MUST NOT fall back to kinds 1 or 11: older firmware's
  private countdowns are not visible in the Timer app. On this firmware, all three kinds
  route to the native app's records; kind 1 maps to create.

  Core Devices Timer UUID is `c64aae42-b822-4d6d-995b-bc7c38e843f4`. The app must be installed
  and closed, and its saved format must match the checked version-1 41-byte records. An
  incompatible store, duplicate id, unknown version, full list, storage failure, or running
  app is refused without guessing. The official phone app is unchanged.

  List returns newline-separated decimal ids; get/create/pause/resume/update/restart return
  `id,state,remaining_ms,duration_seconds\n` (empty label), state 1 running, 2 paused, 3 finished.
  Cancel returns empty text. Eight stored timers fit, including paused/finished timers; create
  never evicts one. Muse ids use a durable sequence below native UI timestamp ids. Native UI
  edits may change an id; list again when not found. Pause preserves remaining time; update
  seconds changes remaining/restart duration, preserving paused state. Restart starts the
  saved duration. Native Timer owns alerts and UI, and the records survive reboot/disconnect.
  Its normal wakeup is armed before an atomic record rewrite; a reset between the two files
  can open Timer early but cannot strand a committed running timer without a wakeup.



`WATCH_ACTION_RESULT` (`0x4A`, watch → phone): `u8 msg_id`, `u8 action_id`, `u8 status`,
`u8 value`, then optionally `u8 text_length` (1–160) and that much UTF-8. `status` 1 done (timer
started, alarm set, option chosen: `value` is its index from 0), 2 declined (the wearer backed
out), 3 timed out, 4 failed, 5 unsupported kind. A receiver must accept the four-byte form.

### 4.6 Marked moments

The wearer can mark a moment as important from the watch (a Quick Launch action). Needs an
authorized receiver; nothing is asked of it but to keep the mark.

`MOMENT_MARK` (`0x4B`, watch → phone): `u8 msg_id`, `u8 mark_id` (counts marks, 1–255 then 1
again; never 0), `u8 flags` (bit0 `RECORDING`), `u8 reserved` (0), `u32 stream_id`,
`u64 sample_index`, `u64 wall_ms`.

- `RECORDING` set: the recording was running, and `stream_id`/`sample_index` are its stream and
  the next sample it would number, so the moment sits on the audio's own clock
  (`start_time_ms + sample_index × 1000 / sample_rate_hz` of that stream). The audio around it
  may arrive later, from the spool.
- `RECORDING` clear: nothing was recording; `stream_id` and `sample_index` are 0, and only
  `wall_ms` (the watch's clock, UTC ms, the same clock as `start_time_ms`) places the moment.

Watches advertising `watch_capabilities` bit6 (`DURABLE_MOMENTS`) persist up to eight
unconfirmed marks, bound to the authorized receiver. They survive restart. A full queue refuses
a new mark visibly; it never discards an older mark. Forgetting the receiver clears its queue.
On authorization/subscription marks are sent oldest first, and pending marks are retried on
subsequent control activity at most once per 30 seconds (a new mark triggers immediate delivery).
No polling timer or microphone activation is needed.

`MOMENT_SAVED` (`0x0F`, phone → watch): `u8 msg_id`, `u8 request_token`, `u8 mark_id`,
`u64 wall_ms` (11 bytes). Send only when bit6 is present, **after** the app durably stores the
mark or recognizes a durable duplicate/deletion tombstone. Echo the original wire `wall_ms`,
not an audio-clock correction. Only an authorized receiver may acknowledge. The watch removes
only the matching `(mark_id, wall_ms)` and returns `ACK`: OK after durable removal (or if already
absent), BUSY when storage failed. Bluetooth acceptance alone never removes a mark.

The notification format remains unchanged. Older firmware has an eight-item RAM queue with
one-shot delivery and no receipts. New apps continue accepting those marks but cannot recover
ones already lost. Install a receipt-capable companion before new firmware: an older app can
receive notifications but cannot drain the durable watch queue.

### 4.7 Phone Focus Quiet Time

Info `watch_capabilities` bit 7 (`CAPABILITY_PHONE_FOCUS`) advertises this extension.
`PHONE_FOCUS_STATE` is phone → watch, exactly 3 bytes: `u8 msg_id=0x10`, `u8 request_token`,
`u8 enabled` (0 or 1 only). Other lengths or values are malformed. An authorized receiver is
required, independently of Muse. The system task applies the state before sending the normal ACK.

This is a separate, volatile Quiet Time source, ORed with manual, scheduled, and calendar-aware
Quiet Time. `enabled=0` never disables another source or changes saved preferences. Manual Off
on the watch may override this source until its next off/on transition; duplicate On requests do
not clear that override. Disconnect, receiver revocation, and reboot clear the phone source.
The phone reasserts the current value on reconnect. Receivers must capability-check before sending;
older firmware must never receive a legacy manual Quiet Time fallback for Focus synchronization.

## 5. Data Channel

One-byte `msg_id` (`0x80–0x9F`) plus packed struct per notification.

| id | name | total size (v1) |
|---|---|---|
| `0x80` | `STREAM_START` | 40 |
| `0x81` | `STREAM_DATA` | 20 + frames |
| `0x82` | `STREAM_GAP` | 26 |
| `0x83` | `STREAM_STOP` | 22 |
| `0x84` | `PTT_AUDIO` | 5 + frames (Section 4.4) |

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
| 20 | 8 | `start_time_ms` — watch wall clock at stream birth, UTC ms |
| 28 | 8 | `start_monotonic_ms` — watch monotonic ms at stream birth |
| 36 | 4 | `flags` — bit0 = RESUME (see below) |

`flags` bit0 (`RESUME`): this `STREAM_START` re-announces an ongoing stream to a freshly
(re)attached receiver rather than beginning a new one. The `stream_id`, codec parameters, and
both start timestamps are the stream-birth values, resent verbatim; sequence numbering continues
(the receiver adopts the first frame's sequence as its contiguity base instead of assuming 0),
and the spool rewinds to the last checkpoint so un-checkpointed frames are re-sent. Receivers
key reattachment off the stable identity (`stream_id` + codec parameters), not the timestamps.

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
2. If Info says Background Audio is disabled and the user wants recording, the phone may write
   `ENABLE_REQUEST`. The watch prompts the user and only persists/enables Background Audio after
   the watch-side approval. The phone then proceeds with authorization.
3. Phone writes `AUTH_REQUEST{receiver_id, name}`. `receiver_id` is 32 random bytes generated
   once per app install, stored in platform-secure storage.
4. Watch behavior:
   - Feature pref disabled → `AUTH_RESULT(denied-disabled)`.
   - No stored receiver → consent prompt on watch → `AUTH_RESULT(pending-user-consent)`;
     on user accept the watch stores SHA-256(receiver_id) + name + timestamp and pushes
     `AUTH_RESULT(ok)`; on decline/timeout (60 s) nothing is stored and a later
     `AUTH_REQUEST` re-prompts.
   - Stored hash matches SHA-256(receiver_id) → `AUTH_RESULT(ok)` immediately.
   - Stored hash differs → `AUTH_RESULT(denied-mismatch)`. Fail closed; the user must revoke
     on the watch before a different install can bind.
5. After `AUTH_RESULT(ok)` the session is authorized for the lifetime of the BLE connection.
   The phone subscribes to Data; the watch streams per policy.
5. Any disconnect ends the session; reconnect repeats the handshake (silent when the hash
   matches). On a bonded reconnect the persisted CCCD subscriptions can be restored before the
   phone re-sends `AUTH_REQUEST`; the session is not ready — and nothing streams — until the
   re-auth completes. If the stream stayed active across the disconnect (brief-disconnect
   bridge), the re-announce after re-auth carries the RESUME flag and the same `stream_id`.
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
| 8 | paused-power-save (watch stationary / low-power policy) |

Codec ids: `0x01` Speex wideband, `0x02` PCM16 debug, `0x03` Opus (reserved), `0x04` LC3
(reserved).

Gap reasons: `0x01` spool overflow, `0x02` mic conflict, `0x03` user disabled,
`0x04` low battery, `0x05` codec error, `0x06` transport reset, `0x07` power save,
`0x08` silence suppressed.

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
