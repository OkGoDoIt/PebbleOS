/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/app_audio_context.h"
#include "pbl/services/app_audio_context_private.h"

#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "drivers/rtc.h"
#include "pbl/services/comm_session/session.h"
#include "process_management/app_manager.h"
#include "system/logging.h"
#include "util/size.h"

#include <string.h>

#define APP_AUDIO_CONTEXT_MAX_PENDING (4)
#define APP_AUDIO_CONTEXT_PENDING_TIMEOUT_SECONDS (30)
#define APP_AUDIO_CONTEXT_MAX_WINDOW_SECONDS (UINT16_MAX)

typedef enum {
  PendingKindNone = 0,
  PendingKindStatus,
  PendingKindTranscript,
  PendingKindSubscription,
} PendingKind;

typedef struct {
  bool active;
  uint16_t request_id;
  PendingKind kind;
  time_t started_at;
} PendingRequest;

static PendingRequest s_pending[APP_AUDIO_CONTEXT_MAX_PENDING];
static uint16_t s_next_request_id = 1;
static AudioContextStatus s_cached_status;
static AudioContextAvailability s_cached_availability = AudioContextAvailabilityUnsupportedPhone;
static uint16_t s_reassembly_request_id;
static uint16_t s_reassembly_expected_parts;
static uint16_t s_reassembly_next_part;
static uint8_t *s_reassembly_payload;
static size_t s_reassembly_length;

static Uuid prv_current_app_uuid(void) {
  return app_manager_get_current_app_md()->uuid;
}

static void prv_fill_header(AppAudioContextHeader *header, AppAudioContextMsgId command_id,
                            uint16_t request_id) {
  *header = (AppAudioContextHeader) {
    .command_id = command_id,
    .protocol_version = APP_AUDIO_CONTEXT_PROTOCOL_VERSION,
    .request_id = request_id,
    .app_uuid = prv_current_app_uuid(),
    .flags = 0,
  };
}

static bool prv_send_packet(const uint8_t *data, size_t length) {
  CommSession *comm_session = comm_session_get_system_session();
  if (!comm_session) {
    return false;
  }
  comm_session_send_data(comm_session, APP_AUDIO_CONTEXT_ENDPOINT, data, length,
                         COMM_SESSION_DEFAULT_TIMEOUT);
  return true;
}

static PendingRequest *prv_find_pending(uint16_t request_id) {
  for (size_t i = 0; i < ARRAY_LENGTH(s_pending); ++i) {
    if (s_pending[i].active && s_pending[i].request_id == request_id) {
      return &s_pending[i];
    }
  }
  return NULL;
}

static bool prv_track_pending(uint16_t request_id, PendingKind kind) {
  const time_t now = rtc_get_time();
  for (size_t i = 0; i < ARRAY_LENGTH(s_pending); ++i) {
    if (s_pending[i].active &&
        (now - s_pending[i].started_at) > APP_AUDIO_CONTEXT_PENDING_TIMEOUT_SECONDS) {
      s_pending[i] = (PendingRequest) {};
    }
  }
  PendingRequest *existing = prv_find_pending(request_id);
  if (existing) {
    existing->kind = kind;
    existing->started_at = now;
    return true;
  }
  for (size_t i = 0; i < ARRAY_LENGTH(s_pending); ++i) {
    if (!s_pending[i].active) {
      s_pending[i] = (PendingRequest) {
        .active = true,
        .request_id = request_id,
        .kind = kind,
        .started_at = now,
      };
      return true;
    }
  }
  return false;
}

static void prv_clear_pending(uint16_t request_id) {
  PendingRequest *pending = prv_find_pending(request_id);
  if (pending) {
    *pending = (PendingRequest) {};
  }
}

static void prv_clear_reassembly(void) {
  if (s_reassembly_payload) {
    kernel_free(s_reassembly_payload);
  }
  s_reassembly_request_id = 0;
  s_reassembly_expected_parts = 0;
  s_reassembly_next_part = 0;
  s_reassembly_payload = NULL;
  s_reassembly_length = 0;
}

static AudioContextAvailability prv_error_to_availability(uint8_t error_code) {
  switch (error_code) {
    case AppAudioContextErrorUnavailable:
      return AudioContextAvailabilityUnsupportedPhone;
    case AppAudioContextErrorPermissionDenied:
      return AudioContextAvailabilityPermissionDenied;
    case AppAudioContextErrorCapabilityNotDeclared:
      return AudioContextAvailabilityCapabilityNotDeclared;
    case AppAudioContextErrorBackgroundAudioDisabled:
      return AudioContextAvailabilityDisabledByUser;
    case AppAudioContextErrorTranscriptionUnavailable:
      return AudioContextAvailabilityTranscriptionUnavailable;
    case AppAudioContextErrorNoData:
      return AudioContextAvailabilityNoData;
    default:
      return AudioContextAvailabilityError;
  }
}

static void prv_put_event(uint16_t request_id, AudioContextAvailability result,
                          const AudioContextStatus *status, const uint8_t *text,
                          size_t text_length) {
  PebbleAudioContextEventData *data = kernel_malloc(sizeof(*data) + text_length + 1);
  if (!data) {
    return;
  }
  *data = (PebbleAudioContextEventData) {
    .request_id = request_id,
    .result = result,
  };
  if (status) {
    data->status = *status;
  }
  if (text && text_length > 0) {
    memcpy(data->text, text, text_length);
  }
  data->text[text_length] = '\0';
  PebbleEvent event = {
    .type = PEBBLE_AUDIO_CONTEXT_EVENT,
    .audio_context = {
      .data = data,
    },
  };
  event_put(&event);
}

static void prv_handle_status_response(const uint8_t *data, size_t size) {
  if (size < sizeof(AppAudioContextStatusResponseMsg)) {
    return;
  }
  const AppAudioContextStatusResponseMsg *msg = (const AppAudioContextStatusResponseMsg *)data;
  AudioContextStatus status = {
    .availability = msg->availability,
    .background_audio_enabled = msg->background_audio_enabled != 0,
    .recording = msg->stream_state == 2,
    .transcribing = msg->transcription_enabled != 0,
    .flags = 0,
  };
  s_cached_status = status;
  s_cached_availability = status.availability;
  prv_put_event(msg->header.request_id, status.availability, &status, NULL, 0);
  prv_clear_pending(msg->header.request_id);
}

static void prv_handle_transcript_response(const uint8_t *data, size_t size) {
  if (size < sizeof(AppAudioContextTranscriptResponseHeader)) {
    return;
  }
  const AppAudioContextTranscriptResponseHeader *msg =
      (const AppAudioContextTranscriptResponseHeader *)data;
  const size_t header_size = sizeof(AppAudioContextTranscriptResponseHeader);
  if (size < header_size + msg->payload_length) {
    return;
  }
  if (msg->part_count <= 1) {
    prv_put_event(msg->header.request_id, msg->status, NULL, data + header_size,
                  msg->payload_length);
    prv_clear_pending(msg->header.request_id);
    return;
  }
  if ((s_reassembly_request_id != msg->header.request_id) || (msg->part_index == 0)) {
    prv_clear_reassembly();
    s_reassembly_request_id = msg->header.request_id;
    s_reassembly_expected_parts = msg->part_count;
  }
  if ((msg->part_index != s_reassembly_next_part) ||
      (msg->part_count != s_reassembly_expected_parts)) {
    prv_clear_reassembly();
    prv_clear_pending(msg->header.request_id);
    return;
  }
  uint8_t *payload = kernel_realloc(s_reassembly_payload, s_reassembly_length + msg->payload_length);
  if (!payload) {
    prv_clear_reassembly();
    prv_clear_pending(msg->header.request_id);
    return;
  }
  s_reassembly_payload = payload;
  memcpy(s_reassembly_payload + s_reassembly_length, data + header_size, msg->payload_length);
  s_reassembly_length += msg->payload_length;
  s_reassembly_next_part++;
  if (s_reassembly_next_part >= s_reassembly_expected_parts) {
    prv_put_event(msg->header.request_id, msg->status, NULL, s_reassembly_payload,
                  s_reassembly_length);
    prv_clear_reassembly();
    prv_clear_pending(msg->header.request_id);
  }
}

static void prv_handle_event(const uint8_t *data, size_t size) {
  if (size < sizeof(AppAudioContextEventHeader)) {
    return;
  }
  const AppAudioContextEventHeader *msg = (const AppAudioContextEventHeader *)data;
  const size_t header_size = sizeof(AppAudioContextEventHeader);
  if (size < header_size + msg->payload_length) {
    return;
  }
  prv_put_event(msg->header.request_id, AudioContextAvailabilityAvailable, NULL,
                data + header_size, msg->payload_length);
}

static void prv_handle_error_response(const uint8_t *data, size_t size) {
  if (size < sizeof(AppAudioContextErrorResponseHeader)) {
    return;
  }
  const AppAudioContextErrorResponseHeader *msg = (const AppAudioContextErrorResponseHeader *)data;
  const AudioContextAvailability availability = prv_error_to_availability(msg->error_code);
  prv_put_event(msg->header.request_id, availability, NULL, NULL, 0);
  prv_clear_pending(msg->header.request_id);
}

void app_audio_context_init(void) {
  memset(s_pending, 0, sizeof(s_pending));
  prv_clear_reassembly();
  s_cached_status = (AudioContextStatus) {
    .availability = AudioContextAvailabilityUnsupportedPhone,
  };
  s_cached_availability = AudioContextAvailabilityUnsupportedPhone;
}

AudioContextAvailability app_audio_context_get_cached_status(AudioContextStatus *status_out) {
  if (status_out) {
    *status_out = s_cached_status;
  }
  return s_cached_availability;
}

uint16_t app_audio_context_next_request_id(void) {
  if (s_next_request_id == 0) {
    s_next_request_id = 1;
  }
  return s_next_request_id++;
}

bool app_audio_context_request_status(uint16_t request_id) {
  AppAudioContextHeader msg;
  prv_fill_header(&msg, AppAudioContextMsgIdStatusRequest, request_id);
  if (!prv_track_pending(request_id, PendingKindStatus)) {
    return false;
  }
  if (!prv_send_packet((const uint8_t *)&msg, sizeof(msg))) {
    prv_clear_pending(request_id);
    return false;
  }
  return true;
}

bool app_audio_context_request_enable(uint16_t request_id) {
  AppAudioContextHeader msg;
  prv_fill_header(&msg, AppAudioContextMsgIdEnablePromptRequest, request_id);
  return prv_send_packet((const uint8_t *)&msg, sizeof(msg));
}

bool app_audio_context_request_permission(uint16_t request_id, AudioContextPermission permissions) {
  uint8_t payload[sizeof(AppAudioContextPermissionRequestHeader) + 5];
  AppAudioContextPermissionRequestHeader *msg = (AppAudioContextPermissionRequestHeader *)payload;
  prv_fill_header(&msg->header, AppAudioContextMsgIdPermissionRequest, request_id);
  msg->permission_count = 0;
  if (permissions & AudioContextPermissionStatus) {
    payload[sizeof(*msg) + msg->permission_count++] = AppAudioContextWirePermissionStatus;
  }
  if (permissions & AudioContextPermissionRecentTranscript) {
    payload[sizeof(*msg) + msg->permission_count++] =
        AppAudioContextWirePermissionRecentTranscript;
  }
  if (permissions & AudioContextPermissionTranscriptHistory) {
    payload[sizeof(*msg) + msg->permission_count++] =
        AppAudioContextWirePermissionTranscriptHistory;
  }
  if (permissions & AudioContextPermissionLiveTranscript) {
    payload[sizeof(*msg) + msg->permission_count++] = AppAudioContextWirePermissionLiveTranscript;
  }
  if (permissions & AudioContextPermissionRawAudio) {
    payload[sizeof(*msg) + msg->permission_count++] = AppAudioContextWirePermissionRawAudio;
  }
  return prv_send_packet(payload, sizeof(*msg) + msg->permission_count);
}

bool app_audio_context_request_recent_transcript(uint16_t request_id, uint32_t before_seconds,
                                                 uint32_t after_seconds) {
  AppAudioContextTranscriptRequestMsg msg = {
    .before_seconds = before_seconds > APP_AUDIO_CONTEXT_MAX_WINDOW_SECONDS ?
        APP_AUDIO_CONTEXT_MAX_WINDOW_SECONDS : before_seconds,
    .after_seconds = after_seconds > APP_AUDIO_CONTEXT_MAX_WINDOW_SECONDS ?
        APP_AUDIO_CONTEXT_MAX_WINDOW_SECONDS : after_seconds,
    .history = 0,
  };
  prv_fill_header(&msg.header, AppAudioContextMsgIdTranscriptRequest, request_id);
  if (!prv_track_pending(request_id, PendingKindTranscript)) {
    return false;
  }
  if (!prv_send_packet((const uint8_t *)&msg, sizeof(msg))) {
    prv_clear_pending(request_id);
    return false;
  }
  return true;
}

bool app_audio_context_request_transcript_history(uint16_t request_id, time_t start_time,
                                                  time_t end_time) {
  AppAudioContextTranscriptRequestMsg msg = {
    .started_at_epoch_ms = (uint64_t)start_time * 1000,
    .ended_at_epoch_ms = (uint64_t)end_time * 1000,
    .history = 1,
  };
  prv_fill_header(&msg.header, AppAudioContextMsgIdTranscriptRequest, request_id);
  if (!prv_track_pending(request_id, PendingKindTranscript)) {
    return false;
  }
  if (!prv_send_packet((const uint8_t *)&msg, sizeof(msg))) {
    prv_clear_pending(request_id);
    return false;
  }
  return true;
}

bool app_audio_context_subscribe_transcript(uint16_t request_id) {
  AppAudioContextSubscribeRequestMsg msg = {
    .include_partial = 1,
  };
  prv_fill_header(&msg.header, AppAudioContextMsgIdSubscribeRequest, request_id);
  if (!prv_track_pending(request_id, PendingKindSubscription)) {
    return false;
  }
  if (!prv_send_packet((const uint8_t *)&msg, sizeof(msg))) {
    prv_clear_pending(request_id);
    return false;
  }
  return true;
}

void app_audio_context_cancel(uint16_t request_id) {
  AppAudioContextCancelRequestMsg msg;
  prv_fill_header(&msg.header, AppAudioContextMsgIdCancelRequest, request_id);
  prv_send_packet((const uint8_t *)&msg, sizeof(msg));
  if (s_reassembly_request_id == request_id) {
    prv_clear_reassembly();
  }
  prv_clear_pending(request_id);
}

void app_audio_context_protocol_msg_callback(CommSession *session, const uint8_t *data,
                                             size_t size) {
  (void)session;
  if (!data || size < sizeof(AppAudioContextHeader)) {
    return;
  }
  const AppAudioContextHeader *header = (const AppAudioContextHeader *)data;
  if (header->protocol_version != APP_AUDIO_CONTEXT_PROTOCOL_VERSION) {
    PBL_LOG_WRN("Unsupported app audio context protocol version %u", header->protocol_version);
    return;
  }
  switch (header->command_id) {
    case AppAudioContextMsgIdStatusResponse:
      prv_handle_status_response(data, size);
      break;
    case AppAudioContextMsgIdTranscriptResponse:
      prv_handle_transcript_response(data, size);
      break;
    case AppAudioContextMsgIdEvent:
      prv_handle_event(data, size);
      break;
    case AppAudioContextMsgIdErrorResponse:
      prv_handle_error_response(data, size);
      break;
    case AppAudioContextMsgIdPromptResponse:
      prv_clear_pending(header->request_id);
      break;
    default:
      PBL_LOG_WRN("Unexpected app audio context msg id %u", header->command_id);
      break;
  }
}
