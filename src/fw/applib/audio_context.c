/* SPDX-License-Identifier: Apache-2.0 */

#include "audio_context.h"

#include "applib/event_service_client.h"
#include "drivers/rtc.h"
#include "pbl/services/app_audio_context.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
  PendingCallbackNone = 0,
  PendingCallbackStatus,
  PendingCallbackTranscript,
  PendingCallbackSubscription,
} PendingCallbackKind;

typedef struct {
  bool active;
  uint16_t request_id;
  PendingCallbackKind kind;
  union {
    AudioContextStatusCallback status;
    AudioContextTranscriptCallback transcript;
  } callback;
  void *context;
} PendingCallback;

static EventServiceInfo s_event_info;
static bool s_event_subscribed;
static PendingCallback s_pending[4];
static uint16_t s_subscription_request_id;
static time_t s_audio_context_launch_time;

static void prv_subscribe_if_needed(void);

static PendingCallback *prv_find_pending(uint16_t request_id) {
  for (unsigned int i = 0; i < 4; ++i) {
    if (s_pending[i].active && s_pending[i].request_id == request_id) {
      return &s_pending[i];
    }
  }
  return NULL;
}

static bool prv_track_status_callback(uint16_t request_id, AudioContextStatusCallback callback,
                                      void *context) {
  for (unsigned int i = 0; i < 4; ++i) {
    if (!s_pending[i].active) {
      s_pending[i] = (PendingCallback) {
        .active = true,
        .request_id = request_id,
        .kind = PendingCallbackStatus,
        .callback.status = callback,
        .context = context,
      };
      return true;
    }
  }
  return false;
}

static bool prv_track_transcript_callback(uint16_t request_id, PendingCallbackKind kind,
                                          AudioContextTranscriptCallback callback, void *context) {
  for (unsigned int i = 0; i < 4; ++i) {
    if (!s_pending[i].active) {
      s_pending[i] = (PendingCallback) {
        .active = true,
        .request_id = request_id,
        .kind = kind,
        .callback.transcript = callback,
        .context = context,
      };
      return true;
    }
  }
  return false;
}

static void prv_clear_pending(uint16_t request_id) {
  PendingCallback *pending = prv_find_pending(request_id);
  if (pending) {
    *pending = (PendingCallback) {};
  }
}

static const char *prv_json_value_after_key(const char *json, const char *key) {
  const char *key_pos = strstr(json, key);
  if (!key_pos) {
    return NULL;
  }
  const char *value = strchr(key_pos + strlen(key), ':');
  return value ? value + 1 : NULL;
}

static time_t prv_json_time_seconds(const char *json, const char *key) {
  const char *value = prv_json_value_after_key(json, key);
  if (!value) {
    return 0;
  }
  return (time_t)(strtoul(value, NULL, 10) / 1000);
}

static uint32_t prv_json_uint(const char *json, const char *key) {
  const char *value = prv_json_value_after_key(json, key);
  if (!value) {
    return 0;
  }
  return (uint32_t)strtoul(value, NULL, 10);
}

static const char *prv_json_string_in_place(char *json, const char *key) {
  char *value = (char *)prv_json_value_after_key(json, key);
  if (!value) {
    return "";
  }
  while (*value == ' ' || *value == '"') {
    value++;
  }
  char *end = value;
  while (*end && *end != '"') {
    end++;
  }
  *end = '\0';
  return value;
}

static AudioContextTranscript prv_transcript_from_payload(char *payload) {
  AudioContextTranscript transcript = {
    .start_time = prv_json_time_seconds(payload, "\"startedAtEpochMs\""),
    .end_time = prv_json_time_seconds(payload, "\"endedAtEpochMs\""),
    .gap_count = prv_json_uint(payload, "\"gapCount\""),
    .flags = 0,
    .text = prv_json_string_in_place(payload, "\"text\""),
  };
  return transcript;
}

static void prv_handle_audio_context_event(PebbleEvent *e, void *context) {
  (void)context;
  PebbleAudioContextEventData *data = e->audio_context.data;
  if (!data) {
    return;
  }
  PendingCallback *pending = prv_find_pending(data->request_id);
  if (!pending) {
    return;
  }
  if (pending->kind == PendingCallbackStatus && pending->callback.status) {
    pending->callback.status(data->result, &data->status, pending->context);
    prv_clear_pending(data->request_id);
    return;
  }
  if ((pending->kind == PendingCallbackTranscript ||
       pending->kind == PendingCallbackSubscription) &&
      pending->callback.transcript) {
    AudioContextTranscript transcript = prv_transcript_from_payload(data->text);
    pending->callback.transcript(data->result, &transcript, pending->context);
    if (pending->kind == PendingCallbackTranscript) {
      prv_clear_pending(data->request_id);
    }
  }
}

static void prv_subscribe_if_needed(void) {
  if (s_event_subscribed) {
    return;
  }
  s_event_info = (EventServiceInfo) {
    .type = PEBBLE_AUDIO_CONTEXT_EVENT,
    .handler = prv_handle_audio_context_event,
  };
  event_service_client_subscribe(&s_event_info);
  s_event_subscribed = true;
}

AudioContextAvailability audio_context_get_cached_status(AudioContextStatus *status_out) {
  return app_audio_context_get_cached_status(status_out);
}

bool audio_context_request_status(AudioContextStatusCallback callback, void *context) {
  if (!callback) {
    return false;
  }
  prv_subscribe_if_needed();
  const uint16_t request_id = app_audio_context_next_request_id();
  if (!prv_track_status_callback(request_id, callback, context)) {
    return false;
  }
  if (!app_audio_context_request_status(request_id)) {
    prv_clear_pending(request_id);
    return false;
  }
  return true;
}

bool audio_context_request_enable(void) {
  return app_audio_context_request_enable(app_audio_context_next_request_id());
}

bool audio_context_request_permission(AudioContextPermission permissions) {
  return app_audio_context_request_permission(app_audio_context_next_request_id(), permissions);
}

bool audio_context_request_recent_transcript(uint32_t before_seconds, uint32_t after_seconds,
                                             AudioContextTranscriptCallback callback,
                                             void *context) {
  if (!callback) {
    return false;
  }
  prv_subscribe_if_needed();
  const uint16_t request_id = app_audio_context_next_request_id();
  if (!prv_track_transcript_callback(request_id, PendingCallbackTranscript, callback, context)) {
    return false;
  }
  if (!app_audio_context_request_recent_transcript(request_id, before_seconds, after_seconds)) {
    prv_clear_pending(request_id);
    return false;
  }
  return true;
}

bool audio_context_request_transcript_history(time_t start_time, time_t end_time,
                                              AudioContextTranscriptCallback callback,
                                              void *context) {
  if (!callback) {
    return false;
  }
  prv_subscribe_if_needed();
  const uint16_t request_id = app_audio_context_next_request_id();
  if (!prv_track_transcript_callback(request_id, PendingCallbackTranscript, callback, context)) {
    return false;
  }
  if (!app_audio_context_request_transcript_history(request_id, start_time, end_time)) {
    prv_clear_pending(request_id);
    return false;
  }
  return true;
}

bool audio_context_get_trigger_info(AudioContextTriggerInfo *info_out) {
  if (!info_out) {
    return false;
  }
  if (s_audio_context_launch_time == 0) {
    s_audio_context_launch_time = rtc_get_time();
  }
  *info_out = (AudioContextTriggerInfo) {
    .launch_reason = app_launch_reason(),
    .launch_button = app_launch_button(),
    .source = AudioContextTriggerSourceWatch,
    .source_action = 0,
    .trigger_time = s_audio_context_launch_time,
    .args = app_launch_get_args(),
  };
  return true;
}

bool audio_context_subscribe_transcript(AudioContextTranscriptCallback callback, void *context) {
  if (!callback || s_subscription_request_id != 0) {
    return false;
  }
  prv_subscribe_if_needed();
  const uint16_t request_id = app_audio_context_next_request_id();
  if (!prv_track_transcript_callback(request_id, PendingCallbackSubscription, callback, context)) {
    return false;
  }
  if (!app_audio_context_subscribe_transcript(request_id)) {
    prv_clear_pending(request_id);
    return false;
  }
  s_subscription_request_id = request_id;
  return true;
}

void audio_context_unsubscribe(void) {
  if (s_subscription_request_id == 0) {
    return;
  }
  app_audio_context_cancel(s_subscription_request_id);
  prv_clear_pending(s_subscription_request_id);
  s_subscription_request_id = 0;
}
