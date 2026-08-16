/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/audio_companion.h"

#include "applib/event_service_client.h"
#include "bluetooth/audio_companion_service.h"
#include "pbl/services/audio_companion_private.h"
#include "pbl/services/battery/battery_state.h"
#include "services/audio_companion/auth.h"
#include "services/audio_companion/reboot_trace.h"
#include "services/audio_companion/spool.h"
#include "system/reboot_reason.h"

#include <pbl/drivers/mic.h>

#include "clar.h"
#include "fake_mutex.h"
#include "fake_new_timer.h"
#include "fake_system_task.h"
#include "stubs_logging.h"
#include "stubs_passert.h"

#include <string.h>
#include <time.h>

void audio_companion_test_reset(void);
PebbleMutex *audio_companion_test_get_lock(void);
void audio_companion_test_force_reboot_trace_capture(void);

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
static bool s_pref_pause_stationary;
static bool s_pref_pause_low_power;
static bool s_pref_silence_suppression;
static uint8_t s_pref_silence_mode;

static AudioCompanionAuthEval s_auth_eval;
static bool s_auth_receiver_exists;
static bool s_auth_store_succeeds;
static bool s_auth_forget_called;
static char s_auth_name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];
static bool s_enable_prompted;

static bool s_voice_speex_initialized;
static int16_t s_voice_frame_buffer[AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES];
static uint8_t s_encoded_counter;

static bool s_mic_running;
static bool s_mic_start_succeeds;
static MicDataHandlerCB s_mic_handler;
static void *s_mic_context;

static uint32_t s_rand32_value;

// ---- Firmware dependency fakes ----

//! The real mic driver holds its own mutex while it calls the data handler, and the handler takes
//! the service lock. Entering the driver with the service lock already held is therefore a lock
//! order inversion: it deadlocked KernelBG mid-dispatch against whichever task was starting or
//! stopping capture, and the watch rebooted on the task watchdog ~7 s later. Assert the invariant
//! on every driver call so no future change can quietly reintroduce it.
static void prv_assert_service_lock_not_held(void) {
  const FakePebbleMutex *lock = (const FakePebbleMutex *)audio_companion_test_get_lock();
  cl_assert(!lock || lock->lock_count == 0);
}

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
  // This call takes bt_lock, which the BT stack holds across HCI round trips and bonding flash
  // I/O. Holding the service lock across it makes s_lock a multi-second lock, and the drain timer
  // callback takes s_lock on the NewTimers task -- the same task that feeds KernelBG's watchdog
  // bit while it is idle. That is a watchdog reset, so pin the ordering here.
  prv_assert_service_lock_not_held();
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

bool shell_prefs_get_audio_companion_pause_stationary_enabled(void) {
  return s_pref_pause_stationary;
}

void shell_prefs_set_audio_companion_pause_stationary_enabled(bool enabled) {
  s_pref_pause_stationary = enabled;
}

bool shell_prefs_get_audio_companion_pause_low_power_enabled(void) {
  return s_pref_pause_low_power;
}

void shell_prefs_set_audio_companion_pause_low_power_enabled(bool enabled) {
  s_pref_pause_low_power = enabled;
}

bool shell_prefs_get_audio_companion_silence_suppression_enabled(void) {
  return s_pref_silence_suppression;
}

void shell_prefs_set_audio_companion_silence_suppression_enabled(bool enabled) {
  s_pref_silence_suppression = enabled;
}

uint8_t shell_prefs_get_audio_companion_silence_mode(void) {
  return s_pref_silence_mode;
}

void shell_prefs_set_audio_companion_silence_mode(uint8_t mode) {
  s_pref_silence_mode = mode;
}

// Reboot-trace persistence + OS reboot reason are stubbed; the pure ring logic
// (reboot_trace.c) is linked in and exercised by test_audio_companion_reboot_trace.c.
static RebootReasonCode s_last_reboot_reason = RebootReasonCode_Unknown;
static RebootReason s_last_reboot_reason_full;
static AudioCompanionRebootTrace s_saved_reboot_trace;

RebootReasonCode reboot_reason_get_last_reboot_reason(void) {
  return s_last_reboot_reason;
}

void reboot_reason_get_last_reboot_reason_full(RebootReason *reason_out) {
  *reason_out = s_last_reboot_reason_full;
}

// Persist across simulated boots so consecutive-fault detection can actually be exercised.
void audio_companion_reboot_trace_load(AudioCompanionRebootTrace *trace) {
  if (audio_companion_reboot_trace_is_valid(&s_saved_reboot_trace)) {
    *trace = s_saved_reboot_trace;
  } else {
    audio_companion_reboot_trace_clear(trace);
  }
}

void audio_companion_reboot_trace_save(const AudioCompanionRebootTrace *trace) {
  s_saved_reboot_trace = *trace;
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
  prv_assert_service_lock_not_held();
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
  prv_assert_service_lock_not_held();
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

static void prv_build_pause(uint8_t *buf, size_t *length_out, uint8_t token, uint8_t reason) {
  const AudioCompanionPauseRequestMsg pause = {
    .msg_id = AudioCompanionCtrlMsgIdPauseRequest,
    .request_token = token,
    .reason = reason,
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

static void prv_build_enable_request(uint8_t *buf, size_t *length_out, uint8_t token) {
  const AudioCompanionEnableRequestMsg request = {
    .msg_id = AudioCompanionCtrlMsgIdEnableRequest,
    .request_token = token,
  };
  memcpy(buf, &request, sizeof(request));
  *length_out = sizeof(request);
}

static void prv_enable_handler(void) {
  s_enable_prompted = true;
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

static void prv_feed_frame_with_sample(int16_t sample) {
  cl_assert(s_mic_handler);
  for (size_t i = 0; i < AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES; i++) {
    s_voice_frame_buffer[i] = sample;
  }
  s_mic_handler(s_voice_frame_buffer, AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES, s_mic_context);
}

static void prv_feed_frame(void) {
  prv_feed_frame_with_sample(500);
}

static void prv_feed_frames(uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    prv_feed_frame();
  }
  fake_system_task_callbacks_invoke_pending();
}

static void prv_feed_silence_frames(uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    prv_feed_frame_with_sample(0);
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
  s_pref_pause_stationary = true;
  s_pref_pause_low_power = true;
  s_pref_silence_suppression = false;
  s_pref_silence_mode = AudioCompanionSilenceModeOff;
  s_auth_eval = AudioCompanionAuthEvalMatch;
  s_auth_receiver_exists = true;
  s_auth_store_succeeds = true;
  s_auth_forget_called = false;
  strcpy(s_auth_name, "Audio App");
  s_enable_prompted = false;
  s_voice_speex_initialized = false;
  s_encoded_counter = 0;
  s_mic_running = false;
  s_mic_start_succeeds = true;
  s_mic_handler = NULL;
  s_mic_context = NULL;
  s_rand32_value = 0x12345678;
  s_last_reboot_reason = RebootReasonCode_Unknown;
  memset(&s_last_reboot_reason_full, 0, sizeof(s_last_reboot_reason_full));
  memset(&s_saved_reboot_trace, 0, sizeof(s_saved_reboot_trace));
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

void test_audio_companion__enable_request_prompts_and_persists_pref(void) {
  audio_companion_set_enable_handler(prv_enable_handler);
  prv_subscribe(false, true);

  uint8_t buf[sizeof(AudioCompanionEnableRequestMsg)];
  size_t length = 0;
  prv_build_enable_request(buf, &length, 0x2D);
  prv_send_control(buf, length);
  cl_assert(s_enable_prompted);
  cl_assert(!audio_companion_is_enabled());

  audio_companion_handle_enable_response(true);
  cl_assert(audio_companion_is_enabled());
  cl_assert(s_pref_enabled);

  const CapturedNotification *ack = prv_last_control_msg(AudioCompanionCtrlMsgIdAck);
  cl_assert(ack);
  AudioCompanionAckMsg ack_msg;
  memcpy(&ack_msg, ack->data, sizeof(ack_msg));
  cl_assert_equal_i(ack_msg.request_token, 0x2D);
  cl_assert_equal_i(ack_msg.status, AudioCompanionAckStatusOk);
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

void test_audio_companion__drain_callbacks_coalesce(void) {
  // EventQueueFull mitigation: the audio path posts a drain on every push threshold and every
  // drain-timer tick. Those posts must coalesce so a burst of frames can never pile drain
  // callbacks onto the shared, finite system-task queue (which the OS reboots on when full).
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  fake_system_task_callbacks_invoke_pending();

  // Feed many frames WITHOUT servicing the system task in between (worst case: KernelBG stalled).
  for (uint32_t i = 0; i < 64; i++) {
    prv_feed_frame();
  }
  // No matter how many frames crossed the push threshold, at most one drain callback is queued.
  cl_assert(fake_system_task_count_callbacks() <= 1);

  fake_system_task_callbacks_invoke_pending();
  // After servicing, the data still flowed through.
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamData));
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

// Out-of-range reconnect with the real iOS ordering: the receiver subscribes BOTH characteristics
// and only then sends AUTH_REQUEST. The watch must still re-announce STREAM_START (with the RESUME
// flag) for the freshly attached session and resend the frames it buffered while disconnected,
// otherwise the phone has no stream context and silently drops every resumed frame.
void test_audio_companion__reconnect_ios_order_reannounces_with_resume_flag(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(8);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  // The fresh stream announces with no RESUME flag.
  const CapturedNotification *fresh = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(fresh);
  AudioCompanionStreamStartMsg fresh_msg;
  memcpy(&fresh_msg, fresh->data, sizeof(fresh_msg));
  cl_assert_equal_i(fresh_msg.flags & AUDIO_COMPANION_STREAM_START_FLAG_RESUME, 0);

  // Out of range: the watch sees the BLE disconnect and falls back to Idle but keeps the stream
  // active (brief-disconnect bridge), buffering further frames into the spool.
  audio_companion_handle_disconnect();
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateIdle);
  prv_feed_frames(4);
  s_data_count = 0;

  // Back in range, iOS order: subscribe both characteristics, THEN authorize.
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  // Draining now re-announces the stream as a RESUME and resends the buffered frames.
  prv_feed_frames(8);
  const CapturedNotification *resumed = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(resumed);
  AudioCompanionStreamStartMsg resumed_msg;
  memcpy(&resumed_msg, resumed->data, sizeof(resumed_msg));
  cl_assert_equal_i(resumed_msg.flags & AUDIO_COMPANION_STREAM_START_FLAG_RESUME,
                    AUDIO_COMPANION_STREAM_START_FLAG_RESUME);
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamData));
}

void test_audio_companion__pause_resume_records_explicit_gap(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(1);
  s_data_count = 0;

  uint8_t buf[sizeof(AudioCompanionPauseRequestMsg)];
  size_t length = 0;
  prv_build_pause(buf, &length, 0x30, AudioCompanionPauseReasonPolicy);
  prv_send_control(buf, length);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPolicy);
  cl_assert(!s_mic_running);
  cl_assert_equal_i(stub_new_timer_get_next(), TIMER_INVALID_ID);

  s_uptime_seconds += 2;
  prv_build_resume(buf, &length, 0x31);
  prv_send_control(buf, length);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  cl_assert(stub_new_timer_get_next() != TIMER_INVALID_ID);

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

void test_audio_companion__user_pause_ends_stream_and_restart_is_fresh(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(8);
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamStart));
  const uint32_t first_stream_id = prv_current_stream_id();

  // An explicit user stop ends the stream and pauses capture.
  uint8_t buf[sizeof(AudioCompanionPauseRequestMsg)];
  size_t length = 0;
  prv_build_pause(buf, &length, 0x40, AudioCompanionPauseReasonUser);
  prv_send_control(buf, length);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPolicy);
  cl_assert(!s_mic_running);
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamStop));

  // Resume starts a brand new stream, not a replay of the pre-stop one.
  s_data_count = 0;
  prv_build_resume(buf, &length, 0x41);
  prv_send_control(buf, length);
  prv_feed_frames(8);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  const uint32_t second_stream_id = prv_current_stream_id();
  cl_assert(second_stream_id != first_stream_id);
}

void test_audio_companion__disconnect_clears_policy_pause(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  // A policy pause keeps the stream but stops capture.
  uint8_t buf[sizeof(AudioCompanionPauseRequestMsg)];
  size_t length = 0;
  prv_build_pause(buf, &length, 0x50, AudioCompanionPauseReasonPolicy);
  prv_send_control(buf, length);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPolicy);

  // The receiver drops the link. The pause must not strand the watch: it falls back to Idle.
  audio_companion_handle_disconnect();
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateIdle);

  // Reconnecting resumes streaming instead of staying stuck in PausedPolicy.
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
}

void test_audio_companion__liveness_watchdog_stops_silent_stream(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(8);  // first drain sends STREAM_START + data
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  const uint32_t stream_id = prv_current_stream_id();

  // No control traffic for longer than the liveness timeout (15 s). The receiver is presumed
  // gone even though the watch never saw a BLE disconnect (shared-link / crashed app).
  s_uptime_seconds += 16;
  prv_feed_frames(8);  // a drain cycle runs the liveness check and trips it
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateAuthorizedIdle);
  cl_assert(!s_mic_running);

  // A control message proves the receiver is back; streaming resumes.
  uint8_t buf[sizeof(AudioCompanionCheckpointMsg)];
  size_t length = 0;
  prv_build_checkpoint(buf, &length, 0x60, stream_id, 0);
  prv_send_control(buf, length);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
}

void test_audio_companion__stationary_runlevel_pauses_with_power_save_gap(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(1);
  s_data_count = 0;

  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
  cl_assert(!s_mic_running);
  cl_assert_equal_i(stub_new_timer_get_next(), TIMER_INVALID_ID);

  s_uptime_seconds += 3;
  audio_companion_set_runlevel(RunLevel_Normal);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);

  prv_feed_frames(8);
  const CapturedNotification *gap = prv_find_data_msg(AudioCompanionDataMsgIdStreamGap);
  cl_assert(gap);
  AudioCompanionStreamGapMsg gap_msg;
  memcpy(&gap_msg, gap->data, sizeof(gap_msg));
  cl_assert_equal_i(gap_msg.reason, AudioCompanionGapReasonPowerSave);
  cl_assert(gap_msg.missing_frame_count >= 1);
}

void test_audio_companion__stationary_pause_is_always_on(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_pause_stationary_enabled(false);
  prv_subscribe(true, true);
  prv_authenticate();

  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
  cl_assert(!s_mic_running);
}

void test_audio_companion__low_power_pause_is_always_on(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_pause_stationary_enabled(true);
  audio_companion_set_pause_low_power_enabled(false);
  prv_subscribe(true, true);
  prv_authenticate();

  audio_companion_set_runlevel(RunLevel_LowPower);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
  cl_assert(!s_mic_running);

  audio_companion_set_runlevel(RunLevel_Normal);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
  cl_assert(!s_mic_running);
}

void test_audio_companion__silence_suppression_sends_gap_only_when_audio_resumes(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_suppression_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();

  // Light mode keeps roughly the first five quiet seconds so quiet speech is not clipped.
  prv_feed_silence_frames(255);
  const uint8_t encoded_before_resume = s_encoded_counter;
  cl_assert(encoded_before_resume > 0);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMax);

  // Once suppression is active, more quiet frames should not create recurring BLE updates.
  s_data_count = 0;
  prv_feed_silence_frames(50);
  cl_assert_equal_i(s_data_count, 0);
  cl_assert_equal_i(s_encoded_counter, encoded_before_resume);

  // Meaningful audio resumes: emit one explicit silence gap, then encode/send the loud frame.
  prv_feed_frame_with_sample(800);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_encoded_counter > encoded_before_resume);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);

  const CapturedNotification *gap = prv_find_data_msg(AudioCompanionDataMsgIdStreamGap);
  cl_assert(gap);
  AudioCompanionStreamGapMsg gap_msg;
  memcpy(&gap_msg, gap->data, sizeof(gap_msg));
  cl_assert_equal_i(gap_msg.reason, AudioCompanionGapReasonSilenceSuppressed);
  cl_assert(gap_msg.missing_frame_count >= 50);
}

void test_audio_companion__light_silence_suppression_resumes_on_quiet_speech(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_mode(AudioCompanionSilenceModeLight);
  prv_subscribe(true, true);
  prv_authenticate();

  prv_feed_silence_frames(255);
  const uint8_t encoded_before_resume = s_encoded_counter;
  cl_assert(encoded_before_resume > 0);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMax);

  s_data_count = 0;
  prv_feed_silence_frames(50);
  cl_assert_equal_i(s_encoded_counter, encoded_before_resume);

  // This is above Light's "definite quiet" threshold but below the old exit threshold, so it
  // used to remain suppressed and drop audible low-level speech until a louder spike arrived.
  prv_feed_frame_with_sample(48);
  fake_system_task_callbacks_invoke_pending();

  cl_assert_equal_i(s_encoded_counter, encoded_before_resume + 1);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);

  const CapturedNotification *gap = prv_find_data_msg(AudioCompanionDataMsgIdStreamGap);
  cl_assert(gap);
  AudioCompanionStreamGapMsg gap_msg;
  memcpy(&gap_msg, gap->data, sizeof(gap_msg));
  cl_assert_equal_i(gap_msg.reason, AudioCompanionGapReasonSilenceSuppressed);
  cl_assert(gap_msg.missing_frame_count >= 50);
}

void test_audio_companion__prefs_loaded_restores_settings_after_boot(void) {
  // setUp ran audio_companion_init() with silence suppression off, mirroring the boot order where
  // the service initializes before the shell prefs file is loaded.
  cl_assert_equal_b(audio_companion_get_silence_suppression_enabled(), false);

  // The prefs file loads later carrying the user's persisted choices; the reload hook must apply
  // them so a reboot does not revert to compile-time defaults.
  s_pref_silence_mode = AudioCompanionSilenceModeBalanced;
  s_pref_pause_stationary = false;
  audio_companion_handle_prefs_loaded();

  cl_assert_equal_b(audio_companion_get_silence_suppression_enabled(), true);
  cl_assert_equal_i(audio_companion_get_silence_mode(), AudioCompanionSilenceModeBalanced);
}

void test_audio_companion__silence_suppression_can_be_disabled(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_suppression_enabled(false);
  prv_subscribe(true, true);
  prv_authenticate();

  prv_feed_silence_frames(170);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  cl_assert_equal_i(s_encoded_counter, 170);
}

// The mic driver invokes the data handler under its own mutex, and the handler takes the service
// lock -- so the driver lock is always acquired before the service lock on the capture path.
// Capture transitions must therefore reach the driver with the service lock released, or KernelBG
// (mid-dispatch, holding the driver mutex, waiting on the service lock) deadlocks against whatever
// task is starting/stopping capture, and the task watchdog reboots the watch. prv_assert_service_
// lock_not_held() in the mic fakes enforces the invariant; these cases drive the transitions that
// arrive from a task other than the one running the mic dispatch.
void test_audio_companion__capture_transitions_never_enter_mic_under_service_lock(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert(s_mic_running);

  // Dictation on the App task: voice calls the conflict hook while KernelBG is dispatching frames.
  prv_feed_frame();
  audio_companion_mic_conflict_begin();
  cl_assert(!s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedConflict);
  audio_companion_mic_conflict_end();
  cl_assert(s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  // Runlevel changes arrive from KernelMain (stationary service, low power).
  prv_feed_frame();
  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert(!s_mic_running);
  audio_companion_set_runlevel(RunLevel_Normal);
  cl_assert(s_mic_running);

  // The settings UI runs on the App task.
  prv_feed_frame();
  audio_companion_set_enabled(false);
  cl_assert(!s_mic_running);
  audio_companion_set_enabled(true);
  cl_assert(s_mic_running);
}

// A mic_start() that loses the race for the driver must settle on PausedConflict instead of
// leaving the applier retrying a start it can never complete.
void test_audio_companion__mic_unavailable_settles_on_conflict_and_recovers(void) {
  audio_companion_set_enabled(true);
  s_mic_start_succeeds = false;
  prv_subscribe(true, true);
  prv_authenticate();

  cl_assert(!s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedConflict);

  // The latch must not wedge the service: once the mic frees up, a pause/resume cycle retries.
  s_mic_start_succeeds = true;
  audio_companion_mic_conflict_begin();
  audio_companion_mic_conflict_end();
  cl_assert(s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
}

// The boot-time flight recorder is the only thing that survives a watchdog reset, so it has to
// carry the OS's stuck-task evidence -- not just the fault class.
void test_audio_companion__boot_trace_captures_watchdog_stuck_task(void) {
  s_last_reboot_reason = RebootReasonCode_Watchdog;
  s_last_reboot_reason_full = (RebootReason) {
    .code = RebootReasonCode_Watchdog,
    .data8 = { 0x01, 0x03 },  // KernelMain fed the watchdog, KernelBackground did not
    .watchdog = {
      .stuck_task_pc = 0x0801a2c4,
      .stuck_task_lr = 0x0801a1f0,
      .stuck_task_callback = 0x0800e3a8,
    },
  };
  s_pref_enabled = true;

  audio_companion_handle_prefs_loaded();

  AudioCompanionRebootTrace trace;
  audio_companion_get_reboot_trace(&trace);
  const AudioCompanionRebootTraceEntry *last = audio_companion_reboot_trace_newest(&trace);
  cl_assert(last != NULL);
  cl_assert_equal_i(last->reason_code, RebootReasonCode_Watchdog);
  cl_assert_equal_i(last->fault_pc, 0x0801a2c4);
  cl_assert_equal_i(last->fault_lr, 0x0801a1f0);
  cl_assert_equal_i(last->fault_extra, 0x0800e3a8);
  cl_assert(last->flags & AudioCompanionRebootTraceFlagEnabled);
  cl_assert_equal_s(audio_companion_reboot_trace_stuck_task_name(last), "KernelBG");
  // Persisted too, so the detail is still there after the next boot.
  cl_assert_equal_i(s_saved_reboot_trace.total_error_reboots, 1);
}

void test_audio_companion__boot_trace_captures_event_queue_full_detail(void) {
  s_last_reboot_reason = RebootReasonCode_EventQueueFull;
  s_last_reboot_reason_full = (RebootReason) {
    .code = RebootReasonCode_EventQueueFull,
    .event_queue = {
      .push_lr = 0x08001111,
      .current_event = 0x08002222,
      .dropped_event = 0x08003333,
    },
  };

  audio_companion_handle_prefs_loaded();

  AudioCompanionRebootTrace trace;
  audio_companion_get_reboot_trace(&trace);
  const AudioCompanionRebootTraceEntry *last = audio_companion_reboot_trace_newest(&trace);
  cl_assert(last != NULL);
  cl_assert_equal_i(last->fault_pc, 0x08001111);
  cl_assert_equal_i(last->fault_lr, 0x08002222);
  cl_assert_equal_i(last->fault_extra, 0x08003333);
  // No watchdog bitsets recorded for a non-watchdog reset: do not name a bogus stuck task.
  cl_assert(audio_companion_reboot_trace_stuck_task_name(last) == NULL);
}

// A background feature must never be able to push the watch into recovery firmware. Three fault
// boots without a healthy session in between means the crash is reproducing on every boot, so the
// feature stands down rather than riding the watch down to a reflash.
void test_audio_companion__stands_down_after_repeated_fault_boots(void) {
  s_pref_enabled = true;
  s_last_reboot_reason = RebootReasonCode_Watchdog;
  s_last_reboot_reason_full = (RebootReason) { .code = RebootReasonCode_Watchdog };

  // Each boot lands soon after the last, so the crash run is never broken by a healthy session.
  for (int i = 1; i <= AUDIO_COMPANION_FAULT_LOOP_THRESHOLD; i++) {
    s_rtc_time = 1000 + (i * 60);
    audio_companion_test_force_reboot_trace_capture();
    audio_companion_handle_prefs_loaded();
  }

  cl_assert_equal_b(s_pref_enabled, false);
  cl_assert_equal_b(audio_companion_is_enabled(), false);
  cl_assert(!s_mic_running);
  // The run is cleared so re-enabling is not immediately undone on the next boot.
  cl_assert_equal_i(s_saved_reboot_trace.consecutive_fault_boots, 0);
}

void test_audio_companion__healthy_session_breaks_the_fault_run(void) {
  s_pref_enabled = true;
  s_last_reboot_reason = RebootReasonCode_Watchdog;
  s_last_reboot_reason_full = (RebootReason) { .code = RebootReasonCode_Watchdog };

  // Two quick crashes, then one that only came after a long healthy session: a one-off after a
  // day of uptime is not a loop and must not disable the feature.
  const uint32_t times[] = { 1000, 1060, 1120 + (24 * 60 * 60) };
  for (size_t i = 0; i < 3; i++) {
    s_rtc_time = (time_t)times[i];
    audio_companion_test_force_reboot_trace_capture();
    audio_companion_handle_prefs_loaded();
  }

  cl_assert_equal_b(s_pref_enabled, true);
  cl_assert_equal_i(s_saved_reboot_trace.consecutive_fault_boots, 1);
}

void test_audio_companion__benign_reboots_never_stand_down(void) {
  s_pref_enabled = true;
  s_last_reboot_reason = RebootReasonCode_SoftwareUpdate;
  s_last_reboot_reason_full = (RebootReason) { .code = RebootReasonCode_SoftwareUpdate };

  for (int i = 1; i <= AUDIO_COMPANION_FAULT_LOOP_THRESHOLD + 2; i++) {
    s_rtc_time = 1000 + (i * 60);
    audio_companion_test_force_reboot_trace_capture();
    audio_companion_handle_prefs_loaded();
  }

  cl_assert_equal_b(s_pref_enabled, true);
  cl_assert_equal_i(s_saved_reboot_trace.consecutive_fault_boots, 0);
}
