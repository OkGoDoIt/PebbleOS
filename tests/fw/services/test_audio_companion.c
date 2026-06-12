/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/audio_companion.h"

#include "applib/event_service_client.h"
#include "bluetooth/audio_companion_service.h"
#include "pbl/services/audio_companion_private.h"
#include "pbl/services/battery/battery_state.h"
#include "services/audio_companion/auth.h"
#include "services/audio_companion/spool.h"

#include "drivers/mic.h"

#include "clar.h"
#include "fake_mutex.h"
#include "fake_new_timer.h"
#include "fake_system_task.h"
#include "stubs_logging.h"
#include "stubs_passert.h"

#include <string.h>
#include <time.h>

void audio_companion_test_reset(void);

#define MAX_CAPTURED_NOTIFICATIONS (32)
#define MAX_CAPTURED_NOTIFICATION_BYTES (512)
#define TEST_MAX_PERIOD_RUN_FOREVER ((uint16_t)(~0))

typedef struct {
  size_t length;
  uint8_t data[MAX_CAPTURED_NOTIFICATION_BYTES];
} CapturedNotification;

static CapturedNotification s_control_notifications[MAX_CAPTURED_NOTIFICATIONS];
static CapturedNotification s_data_notifications[MAX_CAPTURED_NOTIFICATIONS];
static uint32_t s_control_count;
static uint32_t s_data_count;
static uint16_t s_effective_mtu;
static bool s_notify_data_succeeds;
static bool s_notify_control_succeeds;
static ResponseTimeState s_response_time_state;
static uint16_t s_response_time_period_secs;

static bool s_pref_enabled;
static BatteryChargeState s_battery_state;
static uint32_t s_uptime_seconds;
static time_t s_rtc_time;
static uint16_t s_rtc_ms;

static AudioCompanionAuthEval s_auth_eval;
static bool s_auth_receiver_exists;
static bool s_auth_store_succeeds;
static bool s_auth_forget_called;
static char s_auth_name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];

static bool s_voice_speex_initialized;
static int16_t s_voice_frame_buffer[AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES];
static uint8_t s_encoded_counter;

static bool s_mic_running;
static bool s_mic_start_succeeds;
static MicDataHandlerCB s_mic_handler;
static void *s_mic_context;

static uint32_t s_rand32_value;

// ---- Firmware dependency fakes ----

bool bt_driver_audio_companion_notify_data(const uint8_t *data, size_t length) {
  if (!s_notify_data_succeeds) {
    return false;
  }
  cl_assert(s_data_count < MAX_CAPTURED_NOTIFICATIONS);
  cl_assert(length <= MAX_CAPTURED_NOTIFICATION_BYTES);
  s_data_notifications[s_data_count].length = length;
  memcpy(s_data_notifications[s_data_count].data, data, length);
  s_data_count++;
  return true;
}

bool bt_driver_audio_companion_notify_control(const uint8_t *data, size_t length) {
  if (!s_notify_control_succeeds) {
    return false;
  }
  cl_assert(s_control_count < MAX_CAPTURED_NOTIFICATIONS);
  cl_assert(length <= MAX_CAPTURED_NOTIFICATION_BYTES);
  s_control_notifications[s_control_count].length = length;
  memcpy(s_control_notifications[s_control_count].data, data, length);
  s_control_count++;
  return true;
}

uint16_t bt_driver_audio_companion_get_effective_mtu(void) {
  return s_effective_mtu;
}

void bt_driver_audio_companion_set_response_time(ResponseTimeState state,
                                                 uint16_t max_period_secs) {
  s_response_time_state = state;
  s_response_time_period_secs = max_period_secs;
}

void bt_driver_audio_companion_service_init(void) {
}

void event_service_client_subscribe(EventServiceInfo *service_info) {
}

void event_service_client_unsubscribe(EventServiceInfo *service_info) {
}

BatteryChargeState battery_get_charge_state(void) {
  return s_battery_state;
}

uint32_t time_get_uptime_seconds(void) {
  return s_uptime_seconds;
}

time_t rtc_get_time(void) {
  return s_rtc_time;
}

void rtc_get_time_ms(time_t *out_seconds, uint16_t *out_ms) {
  *out_seconds = s_rtc_time;
  *out_ms = s_rtc_ms;
}

uint32_t rand32(void) {
  return s_rand32_value++;
}

bool shell_prefs_get_audio_companion_enabled(void) {
  return s_pref_enabled;
}

void shell_prefs_set_audio_companion_enabled(bool enabled) {
  s_pref_enabled = enabled;
}

void audio_companion_auth_init(void) {
}

bool audio_companion_auth_receiver_exists(void) {
  return s_auth_receiver_exists;
}

bool audio_companion_auth_get_receiver_name(char *buf, size_t buf_size) {
  if (!s_auth_receiver_exists || !buf || buf_size == 0) {
    return false;
  }
  strncpy(buf, s_auth_name, buf_size - 1);
  buf[buf_size - 1] = '\0';
  return true;
}

AudioCompanionAuthEval audio_companion_auth_evaluate(
    const uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES]) {
  return s_auth_eval;
}

bool audio_companion_auth_store_receiver(
    const uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES], const char *name) {
  if (!s_auth_store_succeeds) {
    return false;
  }
  s_auth_receiver_exists = true;
  strncpy(s_auth_name, name, sizeof(s_auth_name) - 1);
  s_auth_name[sizeof(s_auth_name) - 1] = '\0';
  return true;
}

void audio_companion_auth_forget_receiver(void) {
  s_auth_forget_called = true;
  s_auth_receiver_exists = false;
  memset(s_auth_name, 0, sizeof(s_auth_name));
}

bool voice_speex_init(void) {
  s_voice_speex_initialized = true;
  return true;
}

void voice_speex_deinit(void) {
  s_voice_speex_initialized = false;
}

bool voice_speex_is_initialized(void) {
  return s_voice_speex_initialized;
}

int voice_speex_get_frame_size(void) {
  return AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES;
}

int16_t *voice_speex_get_frame_buffer(void) {
  return s_voice_frame_buffer;
}

int voice_speex_encode_frame(int16_t *samples, uint8_t *encoded_data, size_t max_encoded_size) {
  cl_assert(max_encoded_size >= 4);
  encoded_data[0] = 0x53;
  encoded_data[1] = 0x50;
  encoded_data[2] = 0x58;
  encoded_data[3] = s_encoded_counter++;
  return 4;
}

bool mic_start(MicDevice *this, MicDataHandlerCB data_handler, void *context,
               int16_t *audio_buffer, size_t audio_buffer_len) {
  if (!s_mic_start_succeeds || s_mic_running) {
    return false;
  }
  cl_assert_equal_i(audio_buffer_len, AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES);
  s_mic_handler = data_handler;
  s_mic_context = context;
  s_mic_running = true;
  return true;
}

void mic_stop(MicDevice *this) {
  s_mic_running = false;
}

bool mic_is_running(MicDevice *this) {
  return s_mic_running;
}

// ---- Test helpers ----

static void prv_build_auth_request(uint8_t *buf, size_t *length_out, uint8_t token) {
  const char *name = "Audio App";
  AudioCompanionAuthRequestHeader header = {
    .msg_id = AudioCompanionCtrlMsgIdAuthRequest,
    .proto_version = AUDIO_COMPANION_PROTOCOL_VERSION,
    .request_token = token,
    .name_len = strlen(name),
  };
  for (uint8_t i = 0; i < AUDIO_COMPANION_RECEIVER_ID_BYTES; i++) {
    header.receiver_id[i] = i;
  }
  memcpy(buf, &header, sizeof(header));
  memcpy(buf + sizeof(header), name, header.name_len);
  *length_out = sizeof(header) + header.name_len;
}

static void prv_build_checkpoint(uint8_t *buf, size_t *length_out, uint8_t token,
                                 uint32_t stream_id, uint32_t sequence) {
  const AudioCompanionCheckpointMsg checkpoint = {
    .msg_id = AudioCompanionCtrlMsgIdCheckpoint,
    .request_token = token,
    .stream_id = stream_id,
    .highest_contiguous_sequence_persisted = sequence,
    .persisted_sample_index = (uint64_t)(sequence + 1) * AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES,
  };
  memcpy(buf, &checkpoint, sizeof(checkpoint));
  *length_out = sizeof(checkpoint);
}

static void prv_build_pause(uint8_t *buf, size_t *length_out, uint8_t token) {
  const AudioCompanionPauseRequestMsg pause = {
    .msg_id = AudioCompanionCtrlMsgIdPauseRequest,
    .request_token = token,
    .reason = 3,
  };
  memcpy(buf, &pause, sizeof(pause));
  *length_out = sizeof(pause);
}

static void prv_build_resume(uint8_t *buf, size_t *length_out, uint8_t token) {
  const AudioCompanionResumeRequestMsg resume = {
    .msg_id = AudioCompanionCtrlMsgIdResumeRequest,
    .request_token = token,
  };
  memcpy(buf, &resume, sizeof(resume));
  *length_out = sizeof(resume);
}

static void prv_send_control(const uint8_t *buf, size_t length) {
  audio_companion_handle_control_write(buf, length);
  fake_system_task_callbacks_invoke_pending();
}

static void prv_subscribe(bool data, bool control) {
  audio_companion_handle_subscription_change(data, control);
  fake_system_task_callbacks_invoke_pending();
}

static void prv_authenticate(void) {
  uint8_t buf[64];
  size_t length = 0;
  prv_build_auth_request(buf, &length, 0x21);
  prv_send_control(buf, length);
}

static void prv_feed_frame(void) {
  cl_assert(s_mic_handler);
  s_mic_handler(s_voice_frame_buffer, AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES, s_mic_context);
}

static void prv_feed_frames(uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    prv_feed_frame();
  }
  fake_system_task_callbacks_invoke_pending();
}

static const CapturedNotification *prv_find_data_msg(uint8_t msg_id) {
  for (uint32_t i = 0; i < s_data_count; i++) {
    if (s_data_notifications[i].length > 0 && s_data_notifications[i].data[0] == msg_id) {
      return &s_data_notifications[i];
    }
  }
  return NULL;
}

static const CapturedNotification *prv_last_control_msg(uint8_t msg_id) {
  for (int32_t i = (int32_t)s_control_count - 1; i >= 0; i--) {
    if (s_control_notifications[i].length > 0 && s_control_notifications[i].data[0] == msg_id) {
      return &s_control_notifications[i];
    }
  }
  return NULL;
}

static uint32_t prv_current_stream_id(void) {
  const CapturedNotification *start = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(start);
  AudioCompanionStreamStartMsg msg;
  memcpy(&msg, start->data, sizeof(msg));
  return msg.stream_id;
}

void test_audio_companion__initialize(void) {
  s_control_count = 0;
  s_data_count = 0;
  s_effective_mtu = 256;
  s_notify_data_succeeds = true;
  s_notify_control_succeeds = true;
  s_response_time_state = ResponseTimeMax;
  s_response_time_period_secs = 0;
  s_pref_enabled = false;
  s_battery_state = (BatteryChargeState){ .charge_percent = 80 };
  s_uptime_seconds = 100;
  s_rtc_time = 1000;
  s_rtc_ms = 123;
  s_auth_eval = AudioCompanionAuthEvalMatch;
  s_auth_receiver_exists = true;
  s_auth_store_succeeds = true;
  s_auth_forget_called = false;
  strcpy(s_auth_name, "Audio App");
  s_voice_speex_initialized = false;
  s_encoded_counter = 0;
  s_mic_running = false;
  s_mic_start_succeeds = true;
  s_mic_handler = NULL;
  s_mic_context = NULL;
  s_rand32_value = 0x12345678;
  audio_companion_spool_test_set_heap_free_bytes(UINT32_MAX);
  audio_companion_test_reset();
  audio_companion_init();
}

void test_audio_companion__cleanup(void) {
  audio_companion_test_reset();
  fake_system_task_callbacks_cleanup();
  stub_new_timer_cleanup();
  fake_mutex_reset(false);
  fake_pbl_malloc_check_net_allocs();
  fake_pbl_malloc_clear_tracking();
}

void test_audio_companion__denies_auth_while_disabled(void) {
  prv_subscribe(true, true);
  prv_authenticate();

  const CapturedNotification *auth = prv_last_control_msg(AudioCompanionCtrlMsgIdAuthResult);
  cl_assert(auth);
  AudioCompanionAuthResultMsg msg;
  memcpy(&msg, auth->data, sizeof(msg));
  cl_assert_equal_i(msg.status, AudioCompanionAuthStatusDeniedDisabled);
  cl_assert_equal_i(s_data_count, 0);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateDisabled);
}

void test_audio_companion__streams_only_after_authorized_session(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  cl_assert_equal_i(s_data_count, 0);

  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);

  prv_feed_frames(8);

  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamStart));
  const CapturedNotification *data = prv_find_data_msg(AudioCompanionDataMsgIdStreamData);
  cl_assert(data);
  AudioCompanionStreamDataHeader header;
  memcpy(&header, data->data, sizeof(header));
  cl_assert_equal_i(header.first_sequence, 0);
  cl_assert_equal_i(header.frame_count, 8);
}

void test_audio_companion__checkpoint_trims_durable_frames(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(8);

  uint8_t buf[sizeof(AudioCompanionCheckpointMsg)];
  size_t length = 0;
  prv_build_checkpoint(buf, &length, 0x22, prv_current_stream_id(), 7);
  prv_send_control(buf, length);

  const CapturedNotification *ack = prv_last_control_msg(AudioCompanionCtrlMsgIdAck);
  cl_assert(ack);
  AudioCompanionAckMsg ack_msg;
  memcpy(&ack_msg, ack->data, sizeof(ack_msg));
  cl_assert_equal_i(ack_msg.request_token, 0x22);
  cl_assert_equal_i(ack_msg.status, AudioCompanionAckStatusOk);

  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert_equal_i(stats.frames_queued, 0);
}

void test_audio_companion__reconnect_uses_short_catch_up_burst(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);
  cl_assert_equal_i(s_response_time_period_secs, TEST_MAX_PERIOD_RUN_FOREVER);

  audio_companion_handle_disconnect();
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateIdle);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMax);

  prv_feed_frames(4);
  s_data_count = 0;

  prv_subscribe(false, true);
  prv_authenticate();
  prv_subscribe(true, true);

  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMin);
  cl_assert_equal_i(s_response_time_period_secs, MIN_LATENCY_MODE_TIMEOUT_AUDIO_SECS);

  stub_new_timer_invoke(1);
  fake_system_task_callbacks_invoke_pending();

  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamStart));
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamData));
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);
  cl_assert_equal_i(s_response_time_period_secs, TEST_MAX_PERIOD_RUN_FOREVER);
}

void test_audio_companion__pause_resume_records_explicit_gap(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(1);
  s_data_count = 0;

  uint8_t buf[sizeof(AudioCompanionPauseRequestMsg)];
  size_t length = 0;
  prv_build_pause(buf, &length, 0x30);
  prv_send_control(buf, length);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPolicy);
  cl_assert(!s_mic_running);

  s_uptime_seconds += 2;
  prv_build_resume(buf, &length, 0x31);
  prv_send_control(buf, length);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);

  prv_feed_frames(8);
  const CapturedNotification *gap = prv_find_data_msg(AudioCompanionDataMsgIdStreamGap);
  cl_assert(gap);
  AudioCompanionStreamGapMsg gap_msg;
  memcpy(&gap_msg, gap->data, sizeof(gap_msg));
  cl_assert_equal_i(gap_msg.reason, AudioCompanionGapReasonUserDisabled);
  cl_assert(gap_msg.missing_frame_count >= 1);
}

void test_audio_companion__forget_receiver_stops_and_revokes(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(1);

  audio_companion_forget_receiver();

  cl_assert(s_auth_forget_called);
  cl_assert(!s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateIdle);
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamStop));
  const CapturedNotification *revoked = prv_last_control_msg(AudioCompanionCtrlMsgIdRevoked);
  cl_assert(revoked);
}
