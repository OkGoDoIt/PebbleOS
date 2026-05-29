/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "applib/audio_context.h"
#include "util/attributes.h"
#include "util/uuid.h"

#include <stddef.h>
#include <stdint.h>

#define APP_AUDIO_CONTEXT_PROTOCOL_VERSION (1)
#define APP_AUDIO_CONTEXT_ENDPOINT (12000)
#define APP_AUDIO_CONTEXT_MAX_TRANSCRIPT_PAYLOAD_BYTES (3500)

typedef enum {
  AppAudioContextMsgIdStatusRequest = 0x01,
  AppAudioContextMsgIdStatusResponse = 0x02,
  AppAudioContextMsgIdEnablePromptRequest = 0x03,
  AppAudioContextMsgIdPromptResponse = 0x04,
  AppAudioContextMsgIdPermissionRequest = 0x05,
  AppAudioContextMsgIdTranscriptRequest = 0x06,
  AppAudioContextMsgIdTranscriptResponse = 0x07,
  AppAudioContextMsgIdSubscribeRequest = 0x08,
  AppAudioContextMsgIdEvent = 0x09,
  AppAudioContextMsgIdCancelRequest = 0x0a,
  AppAudioContextMsgIdErrorResponse = 0x0b,
} AppAudioContextMsgId;

typedef enum {
  AppAudioContextErrorUnavailable = 0,
  AppAudioContextErrorPermissionDenied = 1,
  AppAudioContextErrorCapabilityNotDeclared = 2,
  AppAudioContextErrorBackgroundAudioDisabled = 3,
  AppAudioContextErrorTranscriptionUnavailable = 4,
  AppAudioContextErrorNoData = 5,
  AppAudioContextErrorResponseTooLarge = 6,
  AppAudioContextErrorInternal = 7,
} AppAudioContextErrorCode;

typedef enum {
  AppAudioContextWirePermissionStatus = 1,
  AppAudioContextWirePermissionRecentTranscript = 2,
  AppAudioContextWirePermissionTranscriptHistory = 3,
  AppAudioContextWirePermissionLiveTranscript = 4,
  AppAudioContextWirePermissionRawAudio = 5,
} AppAudioContextWirePermission;

typedef struct PACKED {
  uint8_t command_id;
  uint8_t protocol_version;
  uint16_t request_id;
  Uuid app_uuid;
  uint32_t flags;
} AppAudioContextHeader;

typedef struct PACKED {
  AppAudioContextHeader header;
  uint8_t availability;
  uint8_t background_audio_enabled;
  uint8_t stream_state;
  uint8_t transcription_enabled;
  uint8_t storage_state;
  uint16_t current_live_subscribers;
  uint16_t current_raw_subscribers;
} AppAudioContextStatusResponseMsg;

typedef struct PACKED {
  AppAudioContextHeader header;
  uint8_t result;
} AppAudioContextPromptResponseMsg;

typedef struct PACKED {
  AppAudioContextHeader header;
  uint8_t permission_count;
  //! Followed by permission_count bytes.
} AppAudioContextPermissionRequestHeader;

typedef struct PACKED {
  AppAudioContextHeader header;
  uint16_t before_seconds;
  uint16_t after_seconds;
  uint64_t anchor_epoch_ms;
  uint64_t started_at_epoch_ms;
  uint64_t ended_at_epoch_ms;
  uint8_t history;
} AppAudioContextTranscriptRequestMsg;

typedef struct PACKED {
  AppAudioContextHeader header;
  uint8_t status;
  uint16_t part_index;
  uint16_t part_count;
  uint16_t payload_length;
  //! Followed by payload_length UTF-8 bytes.
} AppAudioContextTranscriptResponseHeader;

typedef struct PACKED {
  AppAudioContextHeader header;
  uint8_t include_partial;
  uint64_t started_at_epoch_ms;
} AppAudioContextSubscribeRequestMsg;

typedef struct PACKED {
  AppAudioContextHeader header;
  uint16_t payload_length;
  //! Followed by payload_length UTF-8 bytes.
} AppAudioContextEventHeader;

typedef struct PACKED {
  AppAudioContextHeader header;
} AppAudioContextCancelRequestMsg;

typedef struct PACKED {
  AppAudioContextHeader header;
  uint8_t error_code;
  uint16_t message_length;
  //! Followed by message_length UTF-8 bytes.
} AppAudioContextErrorResponseHeader;

size_t app_audio_context_header_size(void);
size_t app_audio_context_status_response_msg_size(void);
size_t app_audio_context_transcript_response_header_size(void);
