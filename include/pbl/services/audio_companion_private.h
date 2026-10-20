/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/kernel/compiler.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Wire definitions for the Audio Companion GATT protocol, version 1.
//! Normative spec: src/fw/services/audio_companion/PROTOCOL.md (mirrored from
//! the pebble-audio-companion repo). All integers little-endian, structs packed.

#define AUDIO_COMPANION_PROTOCOL_VERSION (1)
#define AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES (200)
#define AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES (320)
#define AUDIO_COMPANION_DEFAULT_SAMPLE_RATE_HZ (16000)
#define AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS (20)
#define AUDIO_COMPANION_DEFAULT_BIT_RATE_BPS (9800)
#define AUDIO_COMPANION_MAX_FRAMES_PER_DATA_MSG (32)
#define AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES (24)
#define AUDIO_COMPANION_RECEIVER_ID_BYTES (32)
#define AUDIO_COMPANION_CONSENT_TIMEOUT_SECONDS (60)
//! A version-1 snapshot is the first 20 bytes; the builder emits only those for info_version 1.
#define AUDIO_COMPANION_INFO_V1_SIZE (20)
#define AUDIO_COMPANION_INFO_SIZE (32)
//! Largest control write the watch accepts: a MUSE_SETUP_TX carrying a 240-byte value, or any
//! other request with room to spare. An ATT MTU of 256 carries 253.
#define AUDIO_COMPANION_MAX_CONTROL_WRITE_BYTES (253)

//! Phone -> watch control message ids (writes to the Control characteristic).
typedef enum {
  AudioCompanionCtrlMsgIdAuthRequest = 0x01,
  AudioCompanionCtrlMsgIdAuthRevoke = 0x02,
  AudioCompanionCtrlMsgIdCheckpoint = 0x03,
  AudioCompanionCtrlMsgIdPauseRequest = 0x04,
  AudioCompanionCtrlMsgIdResumeRequest = 0x05,
  AudioCompanionCtrlMsgIdReceiverHealth = 0x06,
  AudioCompanionCtrlMsgIdEnableRequest = 0x07,
  AudioCompanionCtrlMsgIdPhoneMediaState = 0x08,
  AudioCompanionCtrlMsgIdMuseSetupOpen = 0x09,
  AudioCompanionCtrlMsgIdMuseSetupClose = 0x0A,
  AudioCompanionCtrlMsgIdMuseSetupTx = 0x0B,
  AudioCompanionCtrlMsgIdMuseSetupDisconnect = 0x0C,
  AudioCompanionCtrlMsgIdAssistantUpdate = 0x0D,
  AudioCompanionCtrlMsgIdWatchAction = 0x0E,
} AudioCompanionCtrlMsgId;

//! Watch -> phone control message ids (notifications on the Control characteristic).
typedef enum {
  AudioCompanionCtrlMsgIdAuthResult = 0x41,
  AudioCompanionCtrlMsgIdRevoked = 0x42,
  AudioCompanionCtrlMsgIdAck = 0x43,
  AudioCompanionCtrlMsgIdStateChanged = 0x44,
  AudioCompanionCtrlMsgIdError = 0x45,
  AudioCompanionCtrlMsgIdPhoneMediaQuery = 0x46,
  AudioCompanionCtrlMsgIdMuseSetupRx = 0x47,
  AudioCompanionCtrlMsgIdMuseSetupEvent = 0x48,
  AudioCompanionCtrlMsgIdPttEvent = 0x49,
  AudioCompanionCtrlMsgIdWatchActionResult = 0x4A,
} AudioCompanionCtrlOutMsgId;

//! Watch -> phone data message ids (notifications on the Data characteristic).
typedef enum {
  AudioCompanionDataMsgIdStreamStart = 0x80,
  AudioCompanionDataMsgIdStreamData = 0x81,
  AudioCompanionDataMsgIdStreamGap = 0x82,
  AudioCompanionDataMsgIdStreamStop = 0x83,
  AudioCompanionDataMsgIdPttAudio = 0x84,
} AudioCompanionDataMsgId;

//! Service states surfaced in the Info snapshot and STATE_CHANGED.
typedef enum {
  AudioCompanionServiceStateDisabled = 0,
  AudioCompanionServiceStateIdle = 1,
  AudioCompanionServiceStateAuthorizedIdle = 2,
  AudioCompanionServiceStateStreaming = 3,
  AudioCompanionServiceStatePausedConflict = 4,
  AudioCompanionServiceStatePausedPolicy = 5,
  AudioCompanionServiceStatePausedLowBattery = 6,
  AudioCompanionServiceStateError = 7,
  AudioCompanionServiceStatePausedPowerSave = 8,
} AudioCompanionServiceState;

typedef enum {
  AudioCompanionCodecSpeexWideband = 0x01,
  AudioCompanionCodecPcm16Debug = 0x02,
  AudioCompanionCodecOpusReserved = 0x03,
  AudioCompanionCodecLc3Reserved = 0x04,
} AudioCompanionCodecId;

typedef enum {
  AudioCompanionGapReasonSpoolOverflow = 0x01,
  AudioCompanionGapReasonMicConflict = 0x02,
  AudioCompanionGapReasonUserDisabled = 0x03,
  AudioCompanionGapReasonLowBattery = 0x04,
  AudioCompanionGapReasonCodecError = 0x05,
  AudioCompanionGapReasonTransportReset = 0x06,
  AudioCompanionGapReasonPowerSave = 0x07,
  AudioCompanionGapReasonSilenceSuppressed = 0x08,
} AudioCompanionGapReason;

typedef enum {
  AudioCompanionStopReasonUserDisabled = 0x01,
  AudioCompanionStopReasonPolicy = 0x02,
  AudioCompanionStopReasonError = 0x03,
  AudioCompanionStopReasonShutdown = 0x04,
} AudioCompanionStopReason;

typedef enum {
  AudioCompanionPauseReasonLowStorage = 0x01,
  AudioCompanionPauseReasonUser = 0x02,
  AudioCompanionPauseReasonPolicy = 0x03,
} AudioCompanionPauseReason;

typedef enum {
  AudioCompanionAuthStatusOk = 0,
  AudioCompanionAuthStatusPendingUserConsent = 1,
  AudioCompanionAuthStatusDeniedMismatch = 2,
  AudioCompanionAuthStatusDeniedDisabled = 3,
  AudioCompanionAuthStatusInvalid = 4,
} AudioCompanionAuthStatus;

typedef enum {
  AudioCompanionRevokedReasonUserOnWatch = 1,
  AudioCompanionRevokedReasonAppRequested = 2,
  AudioCompanionRevokedReasonReplaced = 3,
} AudioCompanionRevokedReason;

typedef enum {
  AudioCompanionAckStatusOk = 0,
  AudioCompanionAckStatusRejected = 1,
  AudioCompanionAckStatusBadState = 2,
  //! Try again shortly: the watch could not send right now (out of transport buffers).
  AudioCompanionAckStatusBusy = 3,
} AudioCompanionAckStatus;

typedef enum {
  AudioCompanionErrorCodeMalformed = 1,
  AudioCompanionErrorCodeUnauthorized = 2,
  AudioCompanionErrorCodeInternal = 3,
  AudioCompanionErrorCodeUnsupportedVersion = 4,
} AudioCompanionErrorCode;

#define AUDIO_COMPANION_RECEIVER_FLAG_LOW_STORAGE (1u << 0)
#define AUDIO_COMPANION_RECEIVER_FLAG_PAUSE_REQUESTED (1u << 1)

#define AUDIO_COMPANION_INFO_FLAG_RECEIVER_BOUND (1u << 0)
#define AUDIO_COMPANION_INFO_FLAG_ENABLED (1u << 1)
#define AUDIO_COMPANION_INFO_FLAG_CONSENT_PENDING (1u << 2)
//! send_backpressure_events carries a real count. Firmware that predates the field leaves those
//! bytes zero, which is indistinguishable from "no backpressure" without this bit.
#define AUDIO_COMPANION_INFO_FLAG_BACKPRESSURE_COUNTER (1u << 3)

//! Info watch_capabilities bits.
//! The watch can host Meta's Muse gadget setup service for a pairing window and relay it to the
//! phone (MUSE_SETUP_* control messages).
#define AUDIO_COMPANION_CAPABILITY_MUSE_SETUP (1u << 0)
//! Push-to-talk: PTT_EVENT/PTT_AUDIO from the watch, ASSISTANT_UPDATE from the phone.
#define AUDIO_COMPANION_CAPABILITY_PUSH_TO_TALK (1u << 1)
//! Watch actions the assistant can ask for: WATCH_ACTION / WATCH_ACTION_RESULT.
#define AUDIO_COMPANION_CAPABILITY_WATCH_ACTIONS (1u << 2)

// ---- Push-to-talk (Section 4.4) ----

//! The longest question the watch records; Muse takes voice notes of at most 15 s.
#define AUDIO_COMPANION_PTT_MAX_SECONDS (15)
//! Largest ASSISTANT_UPDATE text per write; fits an ATT MTU of 185 with room to spare.
#define AUDIO_COMPANION_ASSISTANT_TEXT_MAX_BYTES (180)

typedef enum {
  AudioCompanionPttEventStart = 1,
  AudioCompanionPttEventEnd = 2,
  AudioCompanionPttEventCancel = 3,
} AudioCompanionPttEvent;

//! PTT_EVENT Start `detail`: what began the question.
typedef enum {
  AudioCompanionPttTriggerQuickLaunch = 1,
  AudioCompanionPttTriggerInApp = 2,
  AudioCompanionPttTriggerFollowUp = 3,
} AudioCompanionPttTrigger;

//! PTT_EVENT End `detail`.
typedef enum {
  AudioCompanionPttEndReleased = 1,
  AudioCompanionPttEndMaxLength = 2,
} AudioCompanionPttEndReason;

//! PTT_EVENT Cancel `detail`.
typedef enum {
  AudioCompanionPttCancelTooShort = 1,
  AudioCompanionPttCancelCancelled = 2,
  AudioCompanionPttCancelPhoneUnavailable = 3,
  AudioCompanionPttCancelMicUnavailable = 4,
} AudioCompanionPttCancelReason;

typedef enum {
  //! What the assistant heard; replaces any earlier one.
  AudioCompanionAssistantUpdateHeard = 1,
  //! What the assistant is doing ("Searching the web"); replaces any earlier one.
  AudioCompanionAssistantUpdateStatus = 2,
  //! More of the answer, appended to what came before.
  AudioCompanionAssistantUpdateAnswer = 3,
  //! The answer is complete.
  AudioCompanionAssistantUpdateDone = 4,
  //! The turn failed; `detail` is an AudioCompanionAssistantError, text optional.
  AudioCompanionAssistantUpdateError = 5,
} AudioCompanionAssistantUpdateKind;

typedef enum {
  AudioCompanionAssistantErrorNotSetUp = 1,
  AudioCompanionAssistantErrorUnreachable = 2,
  AudioCompanionAssistantErrorNoAnswer = 3,
  AudioCompanionAssistantErrorDidNotCatch = 4,
  AudioCompanionAssistantErrorBusy = 5,
} AudioCompanionAssistantError;

// ---- Watch actions (Section 4.5) ----

//! Largest WATCH_ACTION fragment, and the largest joined payload.
#define AUDIO_COMPANION_WATCH_ACTION_FRAGMENT_MAX_BYTES (180)
#define AUDIO_COMPANION_WATCH_ACTION_PAYLOAD_MAX_BYTES (512)
#define AUDIO_COMPANION_WATCH_ACTION_FLAG_MORE (1u << 0)
#define AUDIO_COMPANION_WATCH_ACTION_LABEL_MAX_BYTES (48)
#define AUDIO_COMPANION_WATCH_ACTION_QUESTION_MAX_BYTES (120)
#define AUDIO_COMPANION_WATCH_ACTION_OPTION_MAX_BYTES (48)
#define AUDIO_COMPANION_WATCH_ACTION_MIN_OPTIONS (2)
#define AUDIO_COMPANION_WATCH_ACTION_MAX_OPTIONS (6)
//! Longest text a WATCH_ACTION_RESULT carries (now playing, an app's name, health numbers).
#define AUDIO_COMPANION_WATCH_ACTION_RESULT_TEXT_MAX_BYTES (160)
#define AUDIO_COMPANION_WATCH_ACTION_APP_NAME_MAX_BYTES (48)
#define AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS (7)
#define AUDIO_COMPANION_WATCH_ACTION_FIND_MAX_SECONDS (30)
#define AUDIO_COMPANION_WATCH_ACTION_NOTICE_MAX_BYTES (120)

typedef enum {
  //! u32 seconds, str label: a countdown on the watch.
  AudioCompanionWatchActionTimer = 1,
  //! u8 hour, u8 minute, str label: a one-off alarm at the next such time.
  AudioCompanionWatchActionAlarm = 2,
  //! u16 timeout_s, str question, u8 count, str option x count: the wearer picks one.
  AudioCompanionWatchActionChoose = 3,
  //! u8 command (AudioCompanionWatchMusicCommand): the phone's music, through the watch.
  AudioCompanionWatchActionMusic = 4,
  //! u8 on, u16 minutes (0: until turned off): Quiet Time.
  AudioCompanionWatchActionQuietTime = 5,
  //! str name: open the watch app whose name matches.
  AudioCompanionWatchActionLaunchApp = 6,
  //! u8 seconds (1-30): buzz and light up, to be found.
  AudioCompanionWatchActionFindWatch = 7,
  //! u8 metric (AudioCompanionWatchHealthMetric), u8 days (1-7): the watch's activity data.
  AudioCompanionWatchActionHealth = 8,
  //! str text (<= 120): a short message from the receiver, shown until dismissed or 15 s.
  AudioCompanionWatchActionNotice = 9,
} AudioCompanionWatchActionKind;

typedef enum {
  AudioCompanionWatchMusicPlay = 1,
  AudioCompanionWatchMusicPause = 2,
  AudioCompanionWatchMusicTogglePlayPause = 3,
  AudioCompanionWatchMusicNext = 4,
  AudioCompanionWatchMusicPrevious = 5,
  AudioCompanionWatchMusicVolumeUp = 6,
  AudioCompanionWatchMusicVolumeDown = 7,
  //! Nothing changes; the result says what is playing.
  AudioCompanionWatchMusicNowPlaying = 8,
} AudioCompanionWatchMusicCommand;

typedef enum {
  AudioCompanionWatchHealthSteps = 1,
  AudioCompanionWatchHealthDistanceMeters = 2,
  AudioCompanionWatchHealthActiveSeconds = 3,
  AudioCompanionWatchHealthActiveKCalories = 4,
  AudioCompanionWatchHealthSleepSeconds = 5,
  //! The latest reading, not a daily value: "bpm,age_seconds".
  AudioCompanionWatchHealthHeartRate = 6,
} AudioCompanionWatchHealthMetric;

typedef enum {
  AudioCompanionWatchActionStatusDone = 1,
  AudioCompanionWatchActionStatusDeclined = 2,
  AudioCompanionWatchActionStatusTimedOut = 3,
  AudioCompanionWatchActionStatusFailed = 4,
  AudioCompanionWatchActionStatusUnsupported = 5,
} AudioCompanionWatchActionStatus;

// ---- Muse gadget setup relay (Section 4.3) ----

#define AUDIO_COMPANION_MUSE_SUFFIX_LEN (6)
//! Largest MUSE_SETUP_TX payload: one notification to the Muse app, which chunks at 160 bytes.
#define AUDIO_COMPANION_MUSE_TX_MAX_BYTES (240)
//! Largest MUSE_SETUP_RX payload per notification; a longer characteristic value is split across
//! notifications with the MORE flag set on all but the last.
#define AUDIO_COMPANION_MUSE_RX_MAX_BYTES (249)
#define AUDIO_COMPANION_MUSE_RX_FLAG_MORE (1u << 0)
#define AUDIO_COMPANION_MUSE_WINDOW_MIN_SECS (30)
#define AUDIO_COMPANION_MUSE_WINDOW_MAX_SECS (600)

//! MUSE_SETUP_CLOSE reasons (phone -> watch): why the app ended the window.
typedef enum {
  AudioCompanionMuseAppCloseReasonPaired = 1,
  AudioCompanionMuseAppCloseReasonCancelled = 2,
  AudioCompanionMuseAppCloseReasonFailed = 3,
} AudioCompanionMuseAppCloseReason;

//! MUSE_SETUP_EVENT event codes (watch -> phone).
typedef enum {
  //! The window is open and the watch is advertising as MuseGadgetXXXXXX; `seconds_left` set.
  AudioCompanionMuseEventOpened = 1,
  //! The Muse app connected to the window; `mtu` set.
  AudioCompanionMuseEventClientConnected = 2,
  //! The Muse app subscribed to (detail 1) or unsubscribed from (detail 0) the setup channel.
  AudioCompanionMuseEventSubscribed = 3,
  //! The Muse app's link ended; `detail` is the HCI reason. The window stays open if time remains.
  AudioCompanionMuseEventClientDisconnected = 4,
  //! The window ended; `detail` is an AudioCompanionMuseCloseReason.
  AudioCompanionMuseEventClosed = 5,
  //! Writes from the Muse app were dropped; `detail` counts them.
  AudioCompanionMuseEventRxOverflow = 6,
  //! The Muse app's ATT MTU changed; `mtu` set.
  AudioCompanionMuseEventMtu = 7,
} AudioCompanionMuseEvent;

//! Why a window closed (MUSE_SETUP_EVENT Closed `detail`).
typedef enum {
  AudioCompanionMuseCloseReasonAppPaired = 1,
  AudioCompanionMuseCloseReasonAppCancelled = 2,
  AudioCompanionMuseCloseReasonAppFailed = 3,
  AudioCompanionMuseCloseReasonTimeout = 4,
  AudioCompanionMuseCloseReasonCancelledOnWatch = 5,
  AudioCompanionMuseCloseReasonDeclinedOnWatch = 6,
  AudioCompanionMuseCloseReasonPhoneLinkLost = 7,
  AudioCompanionMuseCloseReasonCouldNotStart = 8,
  AudioCompanionMuseCloseReasonReceiverGone = 9,
} AudioCompanionMuseCloseReason;

//! STREAM_START flags. RESUME marks a re-announcement of an already-running stream to a freshly
//! (re)attached receiver: the receiver must take the first STREAM_DATA/STREAM_GAP sequence as the
//! stream's contiguity base rather than assuming the stream begins at sequence 0.
#define AUDIO_COMPANION_STREAM_START_FLAG_RESUME (1u << 0)

// ---- Wire structs (exact layouts; see PROTOCOL.md) ----

typedef struct PBL_PACKED {
  uint8_t info_version;
  uint8_t protocol_min;
  uint8_t protocol_max;
  uint8_t service_state;
  uint8_t codec_bitmap;
  uint8_t flags;
  uint16_t reserved0;
  uint32_t watch_capabilities;
  uint32_t fw_version_packed;
  //! Notifications the transport refused since service init. Distinguishes airtime loss (this
  //! climbs while audio is missing: the radio could not keep up) from credit starvation (this
  //! stays flat: the receiver stopped checkpointing, so the spool never freed). Valid only when
  //! AUDIO_COMPANION_INFO_FLAG_BACKPRESSURE_COUNTER is set. Was reserved1.
  uint32_t send_backpressure_events;
  //! info_version >= 2 (32 bytes): the watch's last restart, so the phone can see a crash loop
  //! that the fault-loop guard would otherwise only ever report as "disabled". On 2026-09-09
  //! the watch restarted three times in 100 s, stood down, and the phone's only record of it
  //! was AUTH_RESULT status 3 for two hours. `last_reboot_reason` is a RebootReasonCode;
  //! `fault_boots` is the guard's consecutive-fault counter (0 again after a stand-down);
  //! `error_reboots` is the lifetime fault count, saturated; `stood_down` is 1 when the guard
  //! turned background audio off at this boot; `fault_pc`/`fault_lr` come from the newest
  //! reboot-trace entry (watchdog stuck-task PC/LR, or the fault address).
  uint8_t last_reboot_reason;
  uint8_t fault_boots;
  uint8_t error_reboots;
  uint8_t stood_down;
  uint32_t fault_pc;
  uint32_t fault_lr;
} AudioCompanionInfo;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t proto_version;
  uint8_t request_token;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
  uint8_t name_len;
  //! Followed by name_len bytes of UTF-8 name
} AudioCompanionAuthRequestHeader;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
} AudioCompanionAuthRevokeMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint32_t stream_id;
  uint32_t highest_contiguous_sequence_persisted;
  uint64_t persisted_sample_index;
  uint32_t receiver_flags;
  uint32_t free_storage_hint_kb;
} AudioCompanionCheckpointMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t reason;
} AudioCompanionPauseRequestMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
} AudioCompanionResumeRequestMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
} AudioCompanionEnableRequestMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t battery_pct;
  uint8_t app_state;
  uint32_t queue_depth_frames;
} AudioCompanionReceiverHealthMsg;

//! PHONE_MEDIA_STATE flags: what the phone's own audio system says, as opposed to what its media
//! service (AMS) says. The two disagree when a player publishes "playing" while paused.
#define AUDIO_COMPANION_PHONE_MEDIA_CHECKED (1 << 0)       //!< the phone could check
#define AUDIO_COMPANION_PHONE_MEDIA_OTHER_AUDIO (1 << 1)   //!< another app is playing audio
#define AUDIO_COMPANION_PHONE_MEDIA_PRIMARY_AUDIO (1 << 2) //!< ... and it is non-mixable audio

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t flags;
} AudioCompanionPhoneMediaStateMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
} AudioCompanionPhoneMediaQueryMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint16_t window_secs;
  //! The six uppercase hex digits after "MuseGadget": the gadget identity's suffix.
  char suffix[AUDIO_COMPANION_MUSE_SUFFIX_LEN];
  uint8_t flags;
} AudioCompanionMuseSetupOpenMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t reason;  // AudioCompanionMuseAppCloseReason
} AudioCompanionMuseSetupCloseMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t length;
  //! Followed by length bytes: one value to notify on the setup TX characteristic, verbatim.
} AudioCompanionMuseSetupTxHeader;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint16_t delay_ms;
} AudioCompanionMuseSetupDisconnectMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  //! Increments per characteristic value; the fragments of one value share it.
  uint8_t seq;
  uint8_t flags;  // AUDIO_COMPANION_MUSE_RX_FLAG_MORE
  uint8_t length;
  //! Followed by length bytes of the value the Muse app wrote.
} AudioCompanionMuseSetupRxHeader;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t event;  // AudioCompanionMuseEvent
  uint8_t detail;
  uint16_t mtu;
  uint16_t seconds_left;
} AudioCompanionMuseSetupEventMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  //! 1-255, new for each press; never 0.
  uint8_t turn;
  uint8_t event;   // AudioCompanionPttEvent
  uint8_t detail;  // trigger (Start), AudioCompanionPttEndReason (End), cancel reason (Cancel)
  //! End/Cancel: frames captured in the turn (PTT_AUDIO frames 0 .. frame_count-1).
  uint16_t frame_count;
  //! Start: the longest question the watch records, in seconds.
  uint16_t max_seconds;
} AudioCompanionPttEventMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t turn;
  //! Index of the first frame in this message, from 0 at the press.
  uint16_t first_frame;
  uint8_t frame_count;
  //! Followed by frame_count x { uint16_t length; uint8_t payload[length]; } (Speex frames), the
  //! STREAM_DATA frame entry.
} AudioCompanionPttAudioHeader;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t turn;
  uint8_t kind;    // AudioCompanionAssistantUpdateKind
  uint8_t detail;  // Error: AudioCompanionAssistantError
  uint8_t length;
  //! Followed by length bytes of UTF-8 text.
} AudioCompanionAssistantUpdateHeader;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  //! Chosen by the phone, nonzero; the fragments of one action share it.
  uint8_t action_id;
  uint8_t flags;  // AUDIO_COMPANION_WATCH_ACTION_FLAG_MORE
  uint8_t length;
  //! Followed by length bytes of the action's payload (see Section 4.5).
} AudioCompanionWatchActionHeader;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t action_id;
  uint8_t status;  // AudioCompanionWatchActionStatus
  //! Choose: the chosen option, from 0.
  uint8_t value;
  //! Optionally followed by u8 text_length (1-160) and that much UTF-8 (Section 4.5).
} AudioCompanionWatchActionResultMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t status;
  uint8_t granted_proto_version;
} AudioCompanionAuthResultMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t reason;
} AudioCompanionRevokedMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t status;
} AudioCompanionAckMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t service_state;
} AudioCompanionStateChangedMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t error_code;
  uint32_t detail;
} AudioCompanionErrorMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint8_t protocol_version;
  uint32_t stream_id;
  uint8_t codec_id;
  uint8_t channels;
  uint16_t frame_samples;
  uint32_t sample_rate_hz;
  uint32_t bit_rate_bps;
  uint16_t frame_duration_ms;
  uint64_t start_time_ms;
  uint64_t start_monotonic_ms;
  uint32_t flags;
} AudioCompanionStreamStartMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint32_t stream_id;
  uint32_t first_sequence;
  uint64_t first_sample_index;
  uint8_t frame_count;
  uint16_t flags;
  //! Followed by frame_count * (uint16_t length + payload[length])
} AudioCompanionStreamDataHeader;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint32_t stream_id;
  uint32_t first_missing_sequence;
  uint32_t missing_frame_count;
  uint64_t first_missing_sample_index;
  uint8_t reason;
  uint32_t watch_drop_counter;
} AudioCompanionStreamGapMsg;

typedef struct PBL_PACKED {
  uint8_t msg_id;
  uint32_t stream_id;
  uint8_t reason;
  uint32_t final_sequence;
  uint64_t final_sample_index;
  uint32_t counters_crc_or_zero;
} AudioCompanionStreamStopMsg;

// ---- Parsed control message (phone -> watch) ----

typedef enum {
  AudioCompanionParseResultOk = 0,
  AudioCompanionParseResultMalformed,
  AudioCompanionParseResultUnknown,
} AudioCompanionParseResult;

typedef struct {
  uint8_t proto_version;
  uint8_t request_token;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
  uint8_t name_len;
  char name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];  // NUL-terminated
} AudioCompanionAuthRequest;

typedef struct {
  uint8_t request_token;
  uint8_t length;
  uint8_t data[AUDIO_COMPANION_MUSE_TX_MAX_BYTES];
} AudioCompanionMuseSetupTx;

typedef struct {
  uint8_t request_token;
  uint8_t turn;
  uint8_t kind;
  uint8_t detail;
  uint8_t length;
  char text[AUDIO_COMPANION_ASSISTANT_TEXT_MAX_BYTES];
} AudioCompanionAssistantUpdate;

typedef struct {
  uint8_t request_token;
  uint8_t action_id;
  uint8_t flags;
  uint8_t length;
  uint8_t data[AUDIO_COMPANION_WATCH_ACTION_FRAGMENT_MAX_BYTES];
} AudioCompanionWatchActionFragment;

typedef struct {
  uint8_t msg_id;  // AudioCompanionCtrlMsgId
  union {
    AudioCompanionAssistantUpdate assistant_update;
    AudioCompanionWatchActionFragment watch_action;
    AudioCompanionMuseSetupOpenMsg muse_setup_open;
    AudioCompanionMuseSetupCloseMsg muse_setup_close;
    AudioCompanionMuseSetupTx muse_setup_tx;
    AudioCompanionMuseSetupDisconnectMsg muse_setup_disconnect;
    AudioCompanionAuthRequest auth_request;
    AudioCompanionAuthRevokeMsg auth_revoke;
    AudioCompanionCheckpointMsg checkpoint;
    AudioCompanionPauseRequestMsg pause_request;
    AudioCompanionResumeRequestMsg resume_request;
    AudioCompanionReceiverHealthMsg receiver_health;
    AudioCompanionEnableRequestMsg enable_request;
    AudioCompanionPhoneMediaStateMsg phone_media_state;
  };
} AudioCompanionControlMsg;

// ---- Protocol helpers (protocol.c) ----

//! Parse a phone->watch control write. Tolerates trailing bytes (append-only versioning),
//! rejects short or internally inconsistent messages, reports unknown ids distinctly.
AudioCompanionParseResult audio_companion_protocol_parse_control(
    const uint8_t *data, size_t length, AudioCompanionControlMsg *msg_out);

//! All builders return the number of bytes written.
size_t audio_companion_protocol_build_info(uint8_t *buf, size_t buf_size,
                                           const AudioCompanionInfo *info);
size_t audio_companion_protocol_build_auth_result(uint8_t *buf, size_t buf_size,
                                                  uint8_t request_token, uint8_t status,
                                                  uint8_t granted_proto_version);
size_t audio_companion_protocol_build_revoked(uint8_t *buf, size_t buf_size, uint8_t reason);
size_t audio_companion_protocol_build_phone_media_query(uint8_t *buf, size_t buf_size);
//! One MUSE_SETUP_RX notification: header plus `length` bytes of `data`.
size_t audio_companion_protocol_build_muse_setup_rx(uint8_t *buf, size_t buf_size, uint8_t seq,
                                                    uint8_t flags, const uint8_t *data,
                                                    uint8_t length);
size_t audio_companion_protocol_build_muse_setup_event(uint8_t *buf, size_t buf_size,
                                                       uint8_t event, uint8_t detail,
                                                       uint16_t mtu, uint16_t seconds_left);
size_t audio_companion_protocol_build_ptt_event(uint8_t *buf, size_t buf_size, uint8_t turn,
                                                uint8_t event, uint8_t detail,
                                                uint16_t frame_count, uint16_t max_seconds);
//! Writes only the 5-byte PTT_AUDIO header; the caller appends frame entries.
size_t audio_companion_protocol_build_ptt_audio_header(uint8_t *buf, size_t buf_size,
                                                       uint8_t turn, uint16_t first_frame,
                                                       uint8_t frame_count);
//! `text` may be NULL; longer than AUDIO_COMPANION_WATCH_ACTION_RESULT_TEXT_MAX_BYTES is cut on
//! a character boundary.
size_t audio_companion_protocol_build_watch_action_result(uint8_t *buf, size_t buf_size,
                                                          uint8_t action_id, uint8_t status,
                                                          uint8_t value, const char *text);
size_t audio_companion_protocol_build_ack(uint8_t *buf, size_t buf_size, uint8_t request_token,
                                          uint8_t status);
size_t audio_companion_protocol_build_state_changed(uint8_t *buf, size_t buf_size,
                                                    uint8_t service_state);
size_t audio_companion_protocol_build_error(uint8_t *buf, size_t buf_size, uint8_t error_code,
                                            uint32_t detail);
size_t audio_companion_protocol_build_stream_start(uint8_t *buf, size_t buf_size,
                                                   const AudioCompanionStreamStartMsg *params);
//! Writes only the 20-byte data header; the caller appends frame entries.
size_t audio_companion_protocol_build_stream_data_header(uint8_t *buf, size_t buf_size,
                                                         uint32_t stream_id,
                                                         uint32_t first_sequence,
                                                         uint64_t first_sample_index,
                                                         uint8_t frame_count, uint16_t flags);
size_t audio_companion_protocol_build_stream_gap(uint8_t *buf, size_t buf_size,
                                                 const AudioCompanionStreamGapMsg *params);
size_t audio_companion_protocol_build_stream_stop(uint8_t *buf, size_t buf_size,
                                                  const AudioCompanionStreamStopMsg *params);
