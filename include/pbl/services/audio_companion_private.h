/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/attributes.h"

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
#define AUDIO_COMPANION_INFO_SIZE (20)

//! Phone -> watch control message ids (writes to the Control characteristic).
typedef enum {
  AudioCompanionCtrlMsgIdAuthRequest = 0x01,
  AudioCompanionCtrlMsgIdAuthRevoke = 0x02,
  AudioCompanionCtrlMsgIdCheckpoint = 0x03,
  AudioCompanionCtrlMsgIdPauseRequest = 0x04,
  AudioCompanionCtrlMsgIdResumeRequest = 0x05,
  AudioCompanionCtrlMsgIdReceiverHealth = 0x06,
} AudioCompanionCtrlMsgId;

//! Watch -> phone control message ids (notifications on the Control characteristic).
typedef enum {
  AudioCompanionCtrlMsgIdAuthResult = 0x41,
  AudioCompanionCtrlMsgIdRevoked = 0x42,
  AudioCompanionCtrlMsgIdAck = 0x43,
  AudioCompanionCtrlMsgIdStateChanged = 0x44,
  AudioCompanionCtrlMsgIdError = 0x45,
} AudioCompanionCtrlOutMsgId;

//! Watch -> phone data message ids (notifications on the Data characteristic).
typedef enum {
  AudioCompanionDataMsgIdStreamStart = 0x80,
  AudioCompanionDataMsgIdStreamData = 0x81,
  AudioCompanionDataMsgIdStreamGap = 0x82,
  AudioCompanionDataMsgIdStreamStop = 0x83,
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

// ---- Wire structs (exact layouts; see PROTOCOL.md) ----

typedef struct PACKED {
  uint8_t info_version;
  uint8_t protocol_min;
  uint8_t protocol_max;
  uint8_t service_state;
  uint8_t codec_bitmap;
  uint8_t flags;
  uint16_t reserved0;
  uint32_t watch_capabilities;
  uint32_t fw_version_packed;
  uint32_t reserved1;
} AudioCompanionInfo;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t proto_version;
  uint8_t request_token;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
  uint8_t name_len;
  //! Followed by name_len bytes of UTF-8 name
} AudioCompanionAuthRequestHeader;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
} AudioCompanionAuthRevokeMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint32_t stream_id;
  uint32_t highest_contiguous_sequence_persisted;
  uint64_t persisted_sample_index;
  uint32_t receiver_flags;
  uint32_t free_storage_hint_kb;
} AudioCompanionCheckpointMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t reason;
} AudioCompanionPauseRequestMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t request_token;
} AudioCompanionResumeRequestMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t battery_pct;
  uint8_t app_state;
  uint32_t queue_depth_frames;
} AudioCompanionReceiverHealthMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t status;
  uint8_t granted_proto_version;
} AudioCompanionAuthResultMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t reason;
} AudioCompanionRevokedMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t request_token;
  uint8_t status;
} AudioCompanionAckMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t service_state;
} AudioCompanionStateChangedMsg;

typedef struct PACKED {
  uint8_t msg_id;
  uint8_t error_code;
  uint32_t detail;
} AudioCompanionErrorMsg;

typedef struct PACKED {
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

typedef struct PACKED {
  uint8_t msg_id;
  uint32_t stream_id;
  uint32_t first_sequence;
  uint64_t first_sample_index;
  uint8_t frame_count;
  uint16_t flags;
  //! Followed by frame_count * (uint16_t length + payload[length])
} AudioCompanionStreamDataHeader;

typedef struct PACKED {
  uint8_t msg_id;
  uint32_t stream_id;
  uint32_t first_missing_sequence;
  uint32_t missing_frame_count;
  uint64_t first_missing_sample_index;
  uint8_t reason;
  uint32_t watch_drop_counter;
} AudioCompanionStreamGapMsg;

typedef struct PACKED {
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
  uint8_t msg_id;  // AudioCompanionCtrlMsgId
  union {
    AudioCompanionAuthRequest auth_request;
    AudioCompanionAuthRevokeMsg auth_revoke;
    AudioCompanionCheckpointMsg checkpoint;
    AudioCompanionPauseRequestMsg pause_request;
    AudioCompanionResumeRequestMsg resume_request;
    AudioCompanionReceiverHealthMsg receiver_health;
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
