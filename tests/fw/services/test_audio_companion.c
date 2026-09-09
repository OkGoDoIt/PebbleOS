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
// The service holds data logging uploads on while the mic runs (prv_apply_dls_sends), so the
// run-level table's decision does not silently withhold the analytics heartbeat for the hours
// this fork spends recording in RunLevel_Stationary.
void dls_set_send_enable_run_level(bool setting) {}

#include "stubs_logging.h"
#include "stubs_passert.h"

#include <string.h>
#include <time.h>

void audio_companion_test_reset(void);
struct pbl_mutex *audio_companion_test_get_lock(void);
TimerID audio_companion_test_get_silence_probe_timer(void);
bool audio_companion_test_stream_reannounce_pending(void);
TimerID audio_companion_test_get_power_save_listen_timer(void);
TimerID audio_companion_test_get_capture_retry_timer(void);
void audio_companion_test_force_reboot_trace_capture(void);

//! Sixty-four, not thirty-two: a drain slice now sends up to twelve data batches under backlog,
//! and the silence tests build a backlog of hundreds of frames before they start asserting.
#define MAX_CAPTURED_NOTIFICATIONS (64)
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
static bool s_consent_prompted;

static bool s_voice_speex_initialized;
static int16_t s_voice_frame_buffer[AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES];
static uint8_t s_encoded_counter;
static uint32_t s_encoded_frames;

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
  const struct pbl_mutex *lock = audio_companion_test_get_lock();
  cl_assert(!lock || !pbl_mutex_is_owner(lock));
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
  // s_encoded_counter doubles as a payload byte and wraps at 256, which is fewer frames than
  // arming the silence detector legitimately takes. Count separately for the tests that need to
  // know exactly how many frames survived the detector.
  s_encoded_frames++;
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

static void prv_consent_handler(const char *name) {
  s_consent_prompted = true;
}

static void prv_build_receiver_health(uint8_t *buf, size_t *length_out, uint8_t token) {
  const AudioCompanionReceiverHealthMsg health = {
    .msg_id = AudioCompanionCtrlMsgIdReceiverHealth,
    .request_token = token,
  };
  memcpy(buf, &health, sizeof(health));
  *length_out = sizeof(health);
}

static void prv_send_control(const uint8_t *buf, size_t length) {
  audio_companion_handle_control_write(buf, length);
  fake_system_task_callbacks_invoke_pending();
}

static void prv_subscribe(bool data, bool control) {
  audio_companion_handle_subscription_change(data, control);
  fake_system_task_callbacks_invoke_pending();
}

//! The receiver's idle keepalive, which is what answers a suppressed-silence probe.
static void prv_send_receiver_health(uint8_t token) {
  uint8_t buf[sizeof(AudioCompanionReceiverHealthMsg)];
  size_t length = 0;
  prv_build_receiver_health(buf, &length, token);
  prv_send_control(buf, length);
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

// ---- Silence-detector harness ----
//
// The old silence tests fed literal amplitudes of 0 and 500 and asserted only that *something*
// happened, so they would have passed with the enter threshold set to 1 and with an arming window
// of any length -- including the one that could never complete on a real wrist. Everything below
// drives the detector with levels and frame counts chosen against the shipped constants, so a
// wrong threshold, a wrong window length or a clipped resume makes a test fail rather than pass
// more quietly.
//
// Mirrored deliberately from audio_companion.c: if a constant there changes, these fail and the
// change has to be justified.
#define TEST_LIGHT_QUIET_THRESHOLD (8)
#define TEST_LIGHT_RESUME_THRESHOLD (6)
#define TEST_LIGHT_WINDOW_FRAMES (1000)      // 20,000 ms / 20 ms
#define TEST_LIGHT_REARM_FRAMES (100)        // 2,000 ms / 20 ms
#define TEST_LIGHT_MAX_LOUD_IN_WINDOW (20)   // 1000 - (1000 * 980 / 1000)
#define TEST_BALANCED_QUIET_THRESHOLD (10)
#define TEST_BALANCED_RESUME_THRESHOLD (8)
#define TEST_BALANCED_WINDOW_FRAMES (750)    // 15,000 ms / 20 ms
#define TEST_AGGRESSIVE_QUIET_THRESHOLD (12)
#define TEST_AGGRESSIVE_RESUME_THRESHOLD (10)
#define TEST_AGGRESSIVE_WINDOW_FRAMES (250)  // 5,000 ms / 20 ms
#define TEST_BLE_RELAX_FRAMES (1500)         // 30,000 ms / 20 ms
//! Frames the capture path lets accumulate before it asks for a drain slice ahead of the timer
//! (DRAIN_PUSH_THRESHOLD_FRAMES). Feeding exactly this many is how a test forces one drain.
#define TEST_DRAIN_PUSH_FRAMES (20)
//! Comfortably below every mode's resume threshold, so it is quiet at any level.
// Below every mode's RESUME threshold (6/8/10), not merely below the quiet ones: a fixture at or
// above resume would re-open the stream on the first frame after it suppressed.
#define TEST_QUIET_LEVEL (2)
//! Frames the power-save mute verdict must hear before it is allowed to conclude anything.
#define TEST_POWER_SAVE_LISTEN_MIN_FRAMES (250)

//! A frame whose high-passed mean absolute level is `amplitude`, alternating in sign every sample.
//!
//! Alternating at Nyquist is deliberate: the detector high-passes before it measures, so a DC or
//! very-low-frequency test signal would be attenuated to nothing and every threshold assertion
//! below would be measuring the filter rather than the rule. At Nyquist the one-pole blocker is
//! essentially transparent, so `amplitude` in and `amplitude` out.
static void prv_feed_frame_at_level(uint32_t amplitude) {
  cl_assert(s_mic_handler);
  for (size_t i = 0; i < AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES; i++) {
    s_voice_frame_buffer[i] = (i & 1) ? -(int16_t)amplitude : (int16_t)amplitude;
  }
  s_mic_handler(s_voice_frame_buffer, AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES, s_mic_context);
}

static void prv_feed_frames_at_level(uint32_t amplitude, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    prv_feed_frame_at_level(amplitude);
  }
  fake_system_task_callbacks_invoke_pending();
}

//! A quiet room with one short transient in it: `burst_samples` at `burst`, the rest at `floor`.
//! A door, a chair, a knock against a desk. This is what a real 20 ms frame of "silence" looks
//! like, and the reason a peak-based rule can never suppress anything on a wrist.
static void prv_feed_frame_with_transient(uint32_t floor_level, uint32_t burst,
                                          size_t burst_samples) {
  cl_assert(s_mic_handler);
  cl_assert(burst_samples <= AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES);
  for (size_t i = 0; i < AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES; i++) {
    const uint32_t level = (i < burst_samples) ? burst : floor_level;
    s_voice_frame_buffer[i] = (i & 1) ? -(int16_t)level : (int16_t)level;
  }
  s_mic_handler(s_voice_frame_buffer, AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES, s_mic_context);
}

//! Empty the spool the way the repeating 400 ms drain timer does on a real watch, until it stands
//! itself down and hands the receiver-liveness job to the silence probe.
//!
//! Arming the detector now legitimately takes a thousand frames, which is more than one drain
//! callback's burst cap can send and more notifications than the capture buffer holds -- hence the
//! discard as we go. Returns the armed probe timer, or TIMER_INVALID_ID.
static TimerID prv_drain_until_silence_probe_armed(void) {
  for (int i = 0; i < 64; i++) {
    const TimerID probe = audio_companion_test_get_silence_probe_timer();
    if (probe != TIMER_INVALID_ID && stub_new_timer_is_scheduled(probe)) {
      return probe;
    }
    s_data_count = 0;
    stub_new_timer_invoke(1);
    fake_system_task_callbacks_invoke_pending();
  }
  return TIMER_INVALID_ID;
}

static AudioCompanionDiagnostics prv_diag(void) {
  AudioCompanionDiagnostics diag;
  audio_companion_get_diagnostics(&diag);
  return diag;
}

static void prv_start_capture_with_mode(AudioCompanionSilenceMode mode) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_mode(mode);
  prv_subscribe(true, true);
  prv_authenticate();
  s_encoded_frames = 0;
  s_data_count = 0;
}

//! The most recent gap record of a given reason, so a test can assert on the run it just ended
//! rather than on whichever gap happened to be captured first.
static bool prv_last_gap_with_reason(uint8_t reason, AudioCompanionStreamGapMsg *out) {
  bool found = false;
  for (uint32_t i = 0; i < s_data_count; i++) {
    if (s_data_notifications[i].length < sizeof(AudioCompanionStreamGapMsg) ||
        s_data_notifications[i].data[0] != AudioCompanionDataMsgIdStreamGap) {
      continue;
    }
    AudioCompanionStreamGapMsg msg;
    memcpy(&msg, s_data_notifications[i].data, sizeof(msg));
    if (msg.reason == reason) {
      *out = msg;
      found = true;
    }
  }
  return found;
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
  s_consent_prompted = false;
  s_voice_speex_initialized = false;
  s_encoded_counter = 0;
  s_encoded_frames = 0;
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

  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);

  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamStart));
  const CapturedNotification *data = prv_find_data_msg(AudioCompanionDataMsgIdStreamData);
  cl_assert(data);
  AudioCompanionStreamDataHeader header;
  memcpy(&header, data->data, sizeof(header));
  cl_assert_equal_i(header.first_sequence, 0);
  // The fake encoder's 4-byte frames all fit one notification, so the first batch carries
  // everything the push threshold let accumulate.
  cl_assert_equal_i(header.frame_count, TEST_DRAIN_PUSH_FRAMES);
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
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);

  uint8_t buf[sizeof(AudioCompanionCheckpointMsg)];
  size_t length = 0;
  prv_build_checkpoint(buf, &length, 0x22, prv_current_stream_id(),
                       TEST_DRAIN_PUSH_FRAMES - 1);
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

//! Runs one drain slice and returns how many STREAM_DATA notifications it emitted. A backlog
//! keeps every fed frame over the push threshold, so feeding one frame posts the next drain.
static uint32_t prv_run_drain_slice(void) {
  s_data_count = 0;
  prv_feed_frame();
  fake_system_task_callbacks_invoke_pending();
  uint32_t batches = 0;
  for (uint32_t i = 0; i < s_data_count; i++) {
    if (s_data_notifications[i].data[0] == AudioCompanionDataMsgIdStreamData) {
      batches++;
    }
  }
  return batches;
}

//! A backlog must be allowed to catch up faster than the steady-state slice, or a watch that fell
//! behind never recovers. The widening is additive and only while the transport takes everything
//! offered, so the link is probed rather than flooded.
void test_audio_companion__drain_burst_widens_under_backlog(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  // Build a deep backlog with the transport refusing everything.
  // Deep enough that the ramp reaches its ceiling with a real backlog still behind it: the
  // slices below consume 6+8+10+12+12+12 batches of 32 frames.
  s_notify_data_succeeds = false;
  for (uint32_t i = 0; i < 6000; i++) {
    prv_feed_frame();
  }
  fake_system_task_callbacks_invoke_pending();
  s_notify_data_succeeds = true;

  // Slice 1 also carries STREAM_START; the data batches still obey the base burst.
  cl_assert_equal_i(prv_run_drain_slice(), 6);
  cl_assert_equal_i(prv_run_drain_slice(), 8);
  cl_assert_equal_i(prv_run_drain_slice(), 10);
  cl_assert_equal_i(prv_run_drain_slice(), 12);
  cl_assert_equal_i(prv_run_drain_slice(), 12);  // ceiling holds
  cl_assert_equal_i(prv_run_drain_slice(), 12);
}

//! The transport refusing a notification is the signal that the link is at its limit, so the
//! burst must collapse to the base immediately rather than keep offering over-sized slices.
void test_audio_companion__drain_burst_collapses_on_backpressure(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();

  // Deep enough that the ramp reaches its ceiling with a real backlog still behind it: the
  // slices below consume 6+8+10+12+12 batches of 32 frames.
  s_notify_data_succeeds = false;
  for (uint32_t i = 0; i < 6000; i++) {
    prv_feed_frame();
  }
  fake_system_task_callbacks_invoke_pending();
  s_notify_data_succeeds = true;

  cl_assert_equal_i(prv_run_drain_slice(), 6);
  cl_assert_equal_i(prv_run_drain_slice(), 8);
  cl_assert_equal_i(prv_run_drain_slice(), 10);
  cl_assert_equal_i(prv_run_drain_slice(), 12);
  cl_assert_equal_i(prv_run_drain_slice(), 12);

  // The link stops accepting: the next slice sends nothing and the burst resets.
  s_notify_data_succeeds = false;
  cl_assert_equal_i(prv_run_drain_slice(), 0);
  s_notify_data_succeeds = true;
  cl_assert_equal_i(prv_run_drain_slice(), 6);
}

//! Backpressure is what tells the phone the radio could not keep up, as opposed to the phone
//! having stopped checkpointing. It has to be readable from Info, not just the on-watch menu.
void test_audio_companion__info_reports_send_backpressure_events(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();

  uint8_t buf[AUDIO_COMPANION_INFO_SIZE];
  size_t length = sizeof(buf);
  audio_companion_fill_info(buf, &length);
  cl_assert_equal_i(length, AUDIO_COMPANION_INFO_SIZE);
  AudioCompanionInfo info;
  memcpy(&info, buf, sizeof(info));
  cl_assert_equal_i(info.info_version, 1);
  cl_assert(info.flags & AUDIO_COMPANION_INFO_FLAG_BACKPRESSURE_COUNTER);
  cl_assert_equal_i(info.send_backpressure_events, 0);

  s_notify_data_succeeds = false;
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  s_notify_data_succeeds = true;

  length = sizeof(buf);
  audio_companion_fill_info(buf, &length);
  memcpy(&info, buf, sizeof(info));
  cl_assert(info.send_backpressure_events > 0);
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
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
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
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  const CapturedNotification *resumed = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(resumed);
  AudioCompanionStreamStartMsg resumed_msg;
  memcpy(&resumed_msg, resumed->data, sizeof(resumed_msg));
  cl_assert_equal_i(resumed_msg.flags & AUDIO_COMPANION_STREAM_START_FLAG_RESUME,
                    AUDIO_COMPANION_STREAM_START_FLAG_RESUME);
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamData));
}

// A RESUME re-announcement must describe the same stream: same id AND the original stream-birth
// timestamps. Recomputing start_time_ms/start_monotonic_ms at send time made the receiver's
// matching-parameters reattach test fail on every reconnect, so each transport blip minted a new
// segment on the phone.
void test_audio_companion__reannounce_resends_stream_birth_timestamps(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  const CapturedNotification *fresh = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(fresh);
  AudioCompanionStreamStartMsg fresh_msg;
  memcpy(&fresh_msg, fresh->data, sizeof(fresh_msg));

  // Time moves on before the reconnect; the re-announcement must not pick it up.
  s_uptime_seconds += 45;
  s_rtc_time += 45;
  s_rtc_ms = 500;

  audio_companion_handle_disconnect();
  fake_system_task_callbacks_invoke_pending();
  prv_feed_frames(2);
  s_data_count = 0;

  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);

  const CapturedNotification *resumed = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(resumed);
  AudioCompanionStreamStartMsg resumed_msg;
  memcpy(&resumed_msg, resumed->data, sizeof(resumed_msg));
  cl_assert_equal_i(resumed_msg.flags & AUDIO_COMPANION_STREAM_START_FLAG_RESUME,
                    AUDIO_COMPANION_STREAM_START_FLAG_RESUME);
  cl_assert_equal_i(resumed_msg.stream_id, fresh_msg.stream_id);
  cl_assert(resumed_msg.start_time_ms == fresh_msg.start_time_ms);
  cl_assert(resumed_msg.start_monotonic_ms == fresh_msg.start_monotonic_ms);

  // A genuinely new stream captures fresh timestamps, not the cached ones.
  audio_companion_set_enabled(false);
  fake_system_task_callbacks_invoke_pending();
  s_data_count = 0;
  audio_companion_set_enabled(true);
  fake_system_task_callbacks_invoke_pending();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  const CapturedNotification *next = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(next);
  AudioCompanionStreamStartMsg next_msg;
  memcpy(&next_msg, next->data, sizeof(next_msg));
  cl_assert(next_msg.stream_id != fresh_msg.stream_id);
  cl_assert(next_msg.start_time_ms > fresh_msg.start_time_ms);
  cl_assert(next_msg.start_monotonic_ms > fresh_msg.start_monotonic_ms);
}

// The real NimBLE event order on a link drop is: one subscribe(TERM) per CCCD, THEN the
// disconnect event. And on a bonded reconnect the persisted CCCDs are restored (subscribe with
// reason RESTORE) BEFORE the app re-authenticates. Authorization dies with the link, so the
// restore-subscribe alone must not make the session ready — no re-announce and no data until
// AUTH arrives, and then the stream resumes with the same id and the RESUME flag.
void test_audio_companion__nimble_order_unsubscribe_then_disconnect_requires_reauth(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  const uint32_t stream_id = prv_current_stream_id();

  // Real order: CCCD teardown first, then the disconnect callback.
  prv_subscribe(false, false);
  audio_companion_handle_disconnect();
  fake_system_task_callbacks_invoke_pending();
  prv_feed_frames(2);  // brief-disconnect bridge keeps capturing into the spool
  s_data_count = 0;

  // Bonded reconnect: CCCD restore lands before the app writes AUTH. Nothing may stream yet.
  prv_subscribe(true, true);
  prv_feed_frames(2);
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamStart) == NULL);
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamData) == NULL);

  // Authorization arrives; only now does the stream re-announce — same id, RESUME flag.
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  const CapturedNotification *resumed = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(resumed);
  AudioCompanionStreamStartMsg resumed_msg;
  memcpy(&resumed_msg, resumed->data, sizeof(resumed_msg));
  cl_assert_equal_i(resumed_msg.stream_id, stream_id);
  cl_assert_equal_i(resumed_msg.flags & AUDIO_COMPANION_STREAM_START_FLAG_RESUME,
                    AUDIO_COMPANION_STREAM_START_FLAG_RESUME);
}

// The receiver rebuilt its GATT session on a link the watch never saw drop. On iOS this is the
// common case, not the exotic one: the official Pebble app holds the same ACL, so cancelling and
// re-establishing OUR connection (a resync, an app relaunch, iOS reclaiming the app) produces a
// brand-new receiver session with no stream context -- while the watch sees no BLE disconnect and,
// because iOS may not rewrite an already-enabled CCCD, no subscription change either. The only
// thing the watch hears is a fresh AUTH_REQUEST.
//
// An AUTH_REQUEST is BY DEFINITION a receiver with no stream context, so it must re-announce.
// Without this the watch stayed "attached" to a session that no longer exists, never re-sent
// STREAM_START, and streamed frames the phone dropped on the floor for want of a stream context --
// which is the "watch says Streaming, app says waiting for the watch" deadlock, ended only by the
// user toggling Background Audio.
void test_audio_companion__reauth_without_disconnect_reannounces_the_stream(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  const uint32_t stream_id = prv_current_stream_id();

  const CapturedNotification *fresh = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(fresh);
  AudioCompanionStreamStartMsg fresh_msg;
  memcpy(&fresh_msg, fresh->data, sizeof(fresh_msg));
  cl_assert_equal_i(fresh_msg.flags & AUDIO_COMPANION_STREAM_START_FLAG_RESUME, 0);

  // No disconnect. No unsubscribe. Just a new session authorizing over the surviving link.
  s_data_count = 0;
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);

  const CapturedNotification *resumed = prv_find_data_msg(AudioCompanionDataMsgIdStreamStart);
  cl_assert(resumed);
  AudioCompanionStreamStartMsg resumed_msg;
  memcpy(&resumed_msg, resumed->data, sizeof(resumed_msg));
  cl_assert_equal_i(resumed_msg.stream_id, stream_id);
  cl_assert_equal_i(resumed_msg.flags & AUDIO_COMPANION_STREAM_START_FLAG_RESUME,
                    AUDIO_COMPANION_STREAM_START_FLAG_RESUME);
  cl_assert(prv_find_data_msg(AudioCompanionDataMsgIdStreamData));
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
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

  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
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
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
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
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
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

// The liveness watchdog stops TRANSMITTING, not capturing.
//
// A silent receiver is not an absent one. On iOS the app is routinely suspended, jettisoned, or
// relaunched in the background, and comes back seconds later to reattach to this same stream. The
// watchdog used to stop the microphone after fifteen seconds of phone silence, which deadlocked
// the pair: a suspended bluetooth-central app is only ever woken BY the notifications that had
// just stopped, so nothing restarted either end until the user opened the app by hand. That is
// the whole of "the watch barely records anything unless I'm holding the phone".
//
// So the mic keeps running into the spool, exactly as it does across an ordinary BLE disconnect,
// and the audio captured while the receiver was away is delivered when it returns.
void test_audio_companion__liveness_watchdog_keeps_capturing_into_the_spool(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);  // first drain sends STREAM_START + data
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  const uint32_t stream_id = prv_current_stream_id();

  // No control traffic for longer than the liveness timeout (15 s). The receiver is presumed gone
  // even though the watch never saw a BLE disconnect (shared link, suspended app, crashed app).
  s_uptime_seconds += 16;
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);  // a drain cycle runs the liveness check and trips it
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateAuthorizedIdle);
  // The radio stands down (asserted below by the absence of data), but the microphone does not.
  cl_assert(s_mic_running);

  // Everything captured while the receiver was away is still ours to send.
  s_data_count = 0;
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert(s_mic_running);
  cl_assert_equal_i(s_data_count, 0);  // nothing is transmitted to a receiver we think is gone

  // A control message proves the receiver is back; streaming resumes and the buffered audio goes
  // out ahead of anything new.
  uint8_t buf[sizeof(AudioCompanionCheckpointMsg)];
  size_t length = 0;
  prv_build_checkpoint(buf, &length, 0x60, stream_id, 0);
  prv_send_control(buf, length);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  prv_feed_frames(1);
  cl_assert(s_data_count > 0);
}

// Losing the receiver no longer stops the microphone, so spool saturation is the ONLY thing left
// bounding battery on a watch whose phone has gone quiet. That makes this path load-bearing in a
// way it was not before: previously the liveness watchdog stopped capture after fifteen seconds,
// and the park was a second line of defence behind it. Pin it, because a regression here is a
// microphone that runs until the battery is flat with nothing on the other end.
void test_audio_companion__spool_saturation_parks_the_mic_once_the_receiver_is_gone(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);

  // The receiver goes quiet past the liveness window. The mic deliberately keeps running.
  s_uptime_seconds += 16;
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateAuthorizedIdle);
  cl_assert(s_mic_running);

  // Starve the heap so the spool cannot grow past its floor, then keep capturing into it. It
  // saturates and starts shedding its OLDEST frames — and the microphone keeps running, because a
  // drop-oldest ring that is full is doing exactly its job: holding the most recent window. This
  // used to park on the very first dropped frame, so an outage a second longer than the ring cost
  // everything after it, and nothing un-parked capture until a receiver reattached.
  audio_companion_spool_test_set_heap_free_bytes(0);
  for (uint32_t i = 0; i < 400; i++) {
    prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  }
  cl_assert(s_mic_running);

  // The audio that fell out of the spool is reported as loss, not silently forgotten.
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert(stats.dropped_overflow_frames > 0);
  cl_assert(audio_companion_spool_has_pending_gap());

  // Capture stops only after a LONG absence — long enough to outlast any iOS suspension, memory
  // jettison or walk out of range, and short enough not to run the mic all night for a phone that
  // has been switched off.
  s_uptime_seconds += (10 * 60) + 1;
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert(!s_mic_running);
}

// Stationary pausing is ON, automatically, with no setting — stopping the microphone releases the
// PDM rails, buffers and clocks that silence suppression cannot, and the Session 17 battery audit
// found that to be the largest win available. What it must not do is trust wrist motion ALONE.
//
// Thirty motionless minutes describes a nightstand, but it equally describes a meeting, a lecture
// or a film, and the watch was switching its microphone off through all of them — then staying off
// until someone shook their wrist. So the gate asks for the other half of the evidence the watch
// already computes: still AND quiet.
void test_audio_companion__stationary_needs_quiet_as_well_as_stillness(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_mode(AudioCompanionSilenceModeLight);
  prv_subscribe(true, true);
  prv_authenticate();

  // Someone is talking. Sitting still is not a reason to stop recording them.
  prv_feed_frames(4);
  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);

  // The room goes quiet and the detector engages. NOW stillness means nothing worth recording.
  audio_companion_set_runlevel(RunLevel_Normal);
  prv_feed_silence_frames(1005);
  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
  cl_assert(!s_mic_running);

  // Coming back reports the pause honestly, as a gap. Cleared first: the quiet that triggered the
  // mute already produced a SilenceSuppressed gap, and this assertion is about the power-save one.
  s_data_count = 0;
  s_uptime_seconds += 3;
  audio_companion_set_runlevel(RunLevel_Normal);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  bool saw_power_save_gap = false;
  for (uint32_t i = 0; i < s_data_count; i++) {
    if (s_data_notifications[i].length < sizeof(AudioCompanionStreamGapMsg) ||
        s_data_notifications[i].data[0] != AudioCompanionDataMsgIdStreamGap) {
      continue;
    }
    AudioCompanionStreamGapMsg gap_msg;
    memcpy(&gap_msg, s_data_notifications[i].data, sizeof(gap_msg));
    saw_power_save_gap |= (gap_msg.reason == AudioCompanionGapReasonPowerSave);
  }
  cl_assert(saw_power_save_gap);
}

// The stationary verdict borrows the suppression detector's 98% bar, and measured against the
// real recordings a 6 s window clears that bar only 12% of the time -- a breath or a sleeve is
// enough to fail it -- so a still wrist mostly kept its microphone on all night. Stillness keeps
// accumulating, though, and two motionless hours is a nightstand far more often than a lecture.
// The bar therefore steps down with time spent in Stationary: 98% at first, 95% after half an
// hour there, 90% after ninety minutes. The suppression detector itself is untouched.
void test_audio_companion__the_stationary_verdict_relaxes_with_time_still(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_mode(AudioCompanionSilenceModeLight);
  prv_subscribe(true, true);
  prv_authenticate();

  // A room that is quiet 96% of the time: one loud frame in every twenty-five. That never arms
  // suppression (which wants 98%), and it is exactly the room the old verdict could not sleep in.
  for (uint32_t i = 0; i < TEST_LIGHT_WINDOW_FRAMES + 5; i++) {
    prv_feed_frame_at_level((i % 25 == 0) ? 900 : TEST_QUIET_LEVEL);
  }
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(prv_diag().silence_suppressing, false);

  // Freshly still: the bar is the mode's own 98%, so the microphone stays on and the question is
  // re-asked on the recheck cadence.
  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(s_mic_running);
  const TimerID listen = audio_companion_test_get_power_save_listen_timer();
  cl_assert(listen != TIMER_INVALID_ID);
  cl_assert_equal_b(stub_new_timer_is_scheduled(listen), true);

  // Twenty-nine minutes in Stationary: still the strict bar, still on. (The receiver keeps
  // talking across every jump in time here, so the liveness watchdog -- a different policy --
  // does not trip on a phone this test is not simulating.)
  s_uptime_seconds += 29 * 60;
  prv_send_receiver_health(0x31);
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  // Past half an hour the bar is 95%, and this 96%-quiet room now counts as empty: mute.
  s_uptime_seconds += 2 * 60;
  prv_send_receiver_health(0x32);
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(!s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);

  // The next listen window hears a busier room -- one loud frame in twelve, 92% quiet -- which
  // fails the 95% bar, so the microphone stays on...
  cl_assert_equal_b(stub_new_timer_fire(listen), true);  // window opens
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);
  for (uint32_t i = 0; i < TEST_LIGHT_WINDOW_FRAMES + 5; i++) {
    prv_feed_frame_at_level((i % 12 == 0) ? 900 : TEST_QUIET_LEVEL);
  }
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(stub_new_timer_fire(listen), true);  // window closes: verdict
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);

  // ...until ninety minutes of stillness lowers the bar to 90%, where 92% quiet is a nightstand.
  s_uptime_seconds += 60 * 60;
  prv_send_receiver_health(0x33);
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(!s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);

  // Leaving Stationary forgets the stillness clock: back in Normal, then Stationary again, the
  // strict bar applies from scratch.
  audio_companion_set_runlevel(RunLevel_Normal);
  cl_assert(s_mic_running);
  for (uint32_t i = 0; i < TEST_LIGHT_WINDOW_FRAMES + 5; i++) {
    prv_feed_frame_at_level((i % 25 == 0) ? 900 : TEST_QUIET_LEVEL);
  }
  fake_system_task_callbacks_invoke_pending();
  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert(s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
}

// The way back. A stationary mute used to end only on a shake or a button press, so a conversation
// that began while someone sat still was lost outright rather than merely delayed. The watch now
// reopens the microphone on a slow cadence and asks the detector what it heard.
void test_audio_companion__a_stationary_mute_reopens_the_mic_to_listen(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_mode(AudioCompanionSilenceModeLight);
  prv_subscribe(true, true);
  prv_authenticate();

  prv_feed_silence_frames(1005);
  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
  cl_assert(!s_mic_running);

  const TimerID listen = audio_companion_test_get_power_save_listen_timer();
  cl_assert(listen != TIMER_INVALID_ID);
  cl_assert_equal_b(stub_new_timer_is_scheduled(listen), true);

  // The window opens: the microphone comes back so the detector can form a view.
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  // Still a quiet room at the end of the window, so mute again and keep saving power.
  prv_feed_silence_frames(1005);
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(!s_mic_running);

  // ...but a window that hears speech keeps the microphone, which is the whole point.
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
}

// The three runlevels that are not heuristics and pause unconditionally: the panic/reset path, a
// firmware update, and genuine critical battery.
void test_audio_companion__the_non_negotiable_runlevels_always_stop_the_mic(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(1);

  const RunLevel always_pause[] = {
    RunLevel_BareMinimum, RunLevel_FirmwareUpdate, RunLevel_LowPower,
  };
  for (size_t i = 0; i < (sizeof(always_pause) / sizeof(always_pause[0])); i++) {
    audio_companion_set_runlevel(always_pause[i]);
    cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
    cl_assert(!s_mic_running);
    audio_companion_set_runlevel(RunLevel_Normal);
    cl_assert(s_mic_running);
  }
}

void test_audio_companion__silence_suppression_sends_gap_only_when_audio_resumes(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_suppression_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();

  // Light keeps the whole arming window of quiet before it stops sending, so quiet speech at the
  // head of a stretch is never clipped.
  prv_feed_silence_frames(1005);
  const uint8_t encoded_before_resume = s_encoded_counter;
  cl_assert(encoded_before_resume > 0);
  // The connection interval is deliberately NOT relaxed on the suppression edge any more -- that
  // is gated on the run lasting 30 s, and this run is a few frames old. See
  // test_audio_companion__short_quiet_runs_do_not_renegotiate_the_connection.
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);

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

  prv_feed_silence_frames(1005);
  const uint8_t encoded_before_resume = s_encoded_counter;
  cl_assert(encoded_before_resume > 0);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);

  s_data_count = 0;
  prv_feed_silence_frames(50);
  cl_assert_equal_i(s_encoded_counter, encoded_before_resume);

  // Quiet, audible speech: above the resume threshold but nowhere near the old exit threshold of
  // 64, which used to keep the watch silent through it until a louder spike arrived. Fed as an AC
  // frame because the detector high-passes -- a constant offset at this level is wrist noise, not
  // a voice, and is correctly ignored.
  prv_feed_frame_at_level(48);
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

// While silence suppression is active the watch sends nothing, so the drain timer stops -- and
// the drain timer is the only caller of the receiver-liveness check. The check itself also skips
// while suppressing, on purpose: on iOS a healthy receiver is suspended during the quiet PRECISELY
// because no notifications are arriving to wake it, and stopping capture on a working receiver
// would be far worse than the bug being prevented.
//
// The result was that the watchdog was not merely skipped, it was off. A receiver that really was
// gone -- app force-quit, phone rebooted -- left the microphone running and the state reading
// Streaming for as long as the room stayed quiet. So: probe, and stop only if nobody answers.
void test_audio_companion__silence_probe_asks_before_presuming_the_receiver_gone(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_mode(AudioCompanionSilenceModeLight);
  prv_subscribe(true, true);
  prv_authenticate();

  // Long enough to enter suppression. On a real watch the repeating drain timer is what stands
  // itself down once the spool has emptied; here it has to be fired explicitly.
  prv_feed_silence_frames(1005);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);

  const TimerID probe = prv_drain_until_silence_probe_armed();
  cl_assert(probe != TIMER_INVALID_ID);
  cl_assert_equal_b(stub_new_timer_is_scheduled(probe), true);

  // First firing PROBES: a control notification (which is what wakes a suspended iOS app).
  s_control_count = 0;
  cl_assert_equal_b(stub_new_timer_fire(probe), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(prv_last_control_msg(AudioCompanionCtrlMsgIdStateChanged));
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert_equal_b(stub_new_timer_is_scheduled(probe), true);

  // A receiver that answers is asleep, not gone: capture keeps running and the long cadence
  // resumes.
  prv_send_receiver_health(0x40);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert_equal_b(stub_new_timer_is_scheduled(probe), true);

  // Nobody answers the next one, so the watch stops talking to it — but keeps the microphone, and
  // spools, because an unanswered probe is most often a suspended app that is about to come back.
  // Only spool saturation parks the mic, which is the same policy an ordinary disconnect gets.
  cl_assert_equal_b(stub_new_timer_fire(probe), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert_equal_b(stub_new_timer_fire(probe), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateAuthorizedIdle);
  cl_assert_equal_b(mic_is_running(NULL), true);
}

// ...and audio resuming retires the probe outright: the drain timer is running again, and it
// calls the liveness check itself.
void test_audio_companion__silence_probe_stands_down_when_audio_resumes(void) {
  audio_companion_set_enabled(true);
  audio_companion_set_silence_mode(AudioCompanionSilenceModeLight);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_silence_frames(1005);
  const TimerID probe = prv_drain_until_silence_probe_armed();
  cl_assert(probe != TIMER_INVALID_ID);
  cl_assert_equal_b(stub_new_timer_is_scheduled(probe), true);

  prv_feed_frames(4);
  cl_assert_equal_b(stub_new_timer_is_scheduled(probe), false);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
}

// First pairing, and the only path with a human in the middle of it. A receiver that
// re-authorizes while the watch is still waiting for the person to answer is the SAME receiver on
// a NEW GATT session -- an app relaunch, or a resync on an ACL the watch never saw drop, either of
// which happens easily inside the sixty seconds someone takes to look at their wrist.
//
// The consent answer is addressed by request token. Answering the FIRST requester's token names a
// token the app has already forgotten, so it drops the reply and never authorizes -- and nothing
// clears that, because the consent state is cleared only by a BLE disconnect that by construction
// did not happen. Both sides then wait forever.
void test_audio_companion__consent_answers_the_latest_requester(void) {
  s_auth_eval = AudioCompanionAuthEvalNoReceiver;
  audio_companion_set_consent_handler(prv_consent_handler);
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);

  uint8_t buf[64];
  size_t length = 0;
  prv_build_auth_request(buf, &length, 0x11);
  prv_send_control(buf, length);
  cl_assert_equal_b(s_consent_prompted, true);

  // The app's session is rebuilt mid-prompt and it asks again with a fresh token.
  prv_build_auth_request(buf, &length, 0x22);
  prv_send_control(buf, length);

  s_control_count = 0;
  audio_companion_handle_consent_response(true);
  fake_system_task_callbacks_invoke_pending();

  const CapturedNotification *result = prv_last_control_msg(AudioCompanionCtrlMsgIdAuthResult);
  cl_assert(result);
  AudioCompanionAuthResultMsg msg;
  memcpy(&msg, result->data, sizeof(msg));
  cl_assert_equal_i(msg.status, AudioCompanionAuthStatusOk);
  cl_assert_equal_i(msg.request_token, 0x22);
}

// ...and a DIFFERENT receiver asking mid-prompt must not be able to steal the answer the person
// is about to give to the first one.
void test_audio_companion__consent_ignores_a_different_receiver_mid_prompt(void) {
  s_auth_eval = AudioCompanionAuthEvalNoReceiver;
  audio_companion_set_consent_handler(prv_consent_handler);
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);

  uint8_t buf[64];
  size_t length = 0;
  prv_build_auth_request(buf, &length, 0x11);
  prv_send_control(buf, length);

  // Same message shape, different receiver id.
  prv_build_auth_request(buf, &length, 0x33);
  buf[offsetof(AudioCompanionAuthRequestHeader, receiver_id)] ^= 0xFF;
  prv_send_control(buf, length);

  s_control_count = 0;
  audio_companion_handle_consent_response(true);
  fake_system_task_callbacks_invoke_pending();

  const CapturedNotification *result = prv_last_control_msg(AudioCompanionCtrlMsgIdAuthResult);
  cl_assert(result);
  AudioCompanionAuthResultMsg msg;
  memcpy(&msg, result->data, sizeof(msg));
  cl_assert_equal_i(msg.request_token, 0x11);
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

  // Runlevel changes arrive from KernelMain (stationary service, low power). LowPower rather than
  // Stationary because this test is about LOCK ORDER, and stationary now pauses only when the room
  // is also quiet — which would make the assertion below depend on the silence detector.
  prv_feed_frame();
  audio_companion_set_runlevel(RunLevel_LowPower);
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

// ...and it must recover with NOBODY doing anything, which is the case that actually happens.
//
// The comment on this branch used to claim "the next event to reach prv_reevaluate_locked()
// retries". In steady state there is no such event: re-evaluation needs a subscription change, a
// disconnect, an AUTH/consent/enable, a mic-conflict hook, a runlevel change, a battery event that
// CROSSES a threshold, or a checkpoint whose pause flag CHANGED. The phone's ordinary traffic hits
// none of them -- RECEIVER_HEALTH just ACKs and an unchanged checkpoint returns early. So
// dictation holding the mic for a few seconds, or voice_speex_init() losing a malloc race, became
// a permanent PausedConflict.
void test_audio_companion__a_mic_conflict_retries_itself(void) {
  audio_companion_set_enabled(true);
  s_mic_start_succeeds = false;
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedConflict);

  const TimerID retry = audio_companion_test_get_capture_retry_timer();
  cl_assert(retry != TIMER_INVALID_ID);
  cl_assert_equal_b(stub_new_timer_is_scheduled(retry), true);

  // Still unavailable: it backs off and stays armed rather than giving up.
  cl_assert_equal_b(stub_new_timer_fire(retry), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(!s_mic_running);
  cl_assert_equal_b(stub_new_timer_is_scheduled(retry), true);

  // The mic frees up. No user gesture, no reconnect, no runlevel change -- just the retry.
  s_mic_start_succeeds = true;
  cl_assert_equal_b(stub_new_timer_fire(retry), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert_equal_b(stub_new_timer_is_scheduled(retry), false);
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

// ================================================================================================
// Silence detector
//
// Context for everything below: this feature shipped default-on, was wired end to end, and
// produced exactly ZERO suppressed-silence gaps across 88 days and 116 hours of real recording.
// The cause was arithmetic, not plumbing -- the entry rule asked for 250 CONSECUTIVE 20 ms frames
// under the threshold and reset its counter on the first frame at or above it, and a room that a
// person calls silent still produces a transient every few seconds. These tests pin the rule that
// replaced it, and each is written so that a wrong constant fails rather than passes.
// ================================================================================================

//! Arming takes one whole window and not one frame less. Fails if window_frames moves, if the
//! old off-by-one comes back, or if the window is counted from the wrong place.
void test_audio_companion__light_arms_after_exactly_one_window_of_quiet(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);

  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES - 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);
  cl_assert_equal_i(s_encoded_frames, TEST_LIGHT_WINDOW_FRAMES - 1);

  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);
  // The frame that completes the window is itself skipped: the evidence was already in.
  cl_assert_equal_i(s_encoded_frames, TEST_LIGHT_WINDOW_FRAMES - 1);
  cl_assert_equal_i(prv_diag().suppressed_silence_frames, 1);
  cl_assert_equal_i(prv_diag().silence_runs, 1);
}

//! The 88-day bug, as a regression test. One loud frame in the middle of a quiet stretch used to
//! send the counter back to zero; now it costs one bit out of the window.
void test_audio_companion__a_single_loud_frame_no_longer_restarts_the_quiet_window(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);

  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 100);
  prv_feed_frames_at_level(900, 1);  // a door, a chair, a knock against the desk
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES - 102);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);

  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);
}

//! ...but the tolerance is 2% and not more. Both halves matter: the first assertion fails if the
//! quiet fraction is loosened, the second if it is tightened.
void test_audio_companion__the_quiet_window_tolerates_two_percent_and_no_more(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);

  prv_feed_frames_at_level(900, TEST_LIGHT_MAX_LOUD_IN_WINDOW + 1);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL,
                           TEST_LIGHT_WINDOW_FRAMES - (TEST_LIGHT_MAX_LOUD_IN_WINDOW + 1));
  cl_assert_equal_b(prv_diag().silence_suppressing, false);  // 979/1000 is not 98%

  // One more quiet frame ages the oldest loud one out of the window: 980/1000 is exactly 98%.
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);
}

//! Resume is a SEPARATE and LOWER threshold than quiet, and one frame is always enough.
//!
//! The shipped code resumed at `exit_threshold` (64) -- above the quiet threshold -- after up to
//! five confirming frames. Simulated over 116.5 h of this watch's own recordings that clipped the
//! onset of 52% of utterances following a quiet stretch and destroyed 206 of them outright. With
//! resume below quiet and no confirmation, the same corpus gives zero destroyed and a p99 clip of
//! 0 ms. This test pins both halves: a frame under the resume threshold stays suppressed, and the
//! frame that reaches it is ENCODED rather than spent confirming.
void test_audio_companion__light_resumes_below_the_quiet_threshold_and_keeps_that_frame(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 50);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);

  const uint32_t encoded_before = s_encoded_frames;
  s_data_count = 0;

  prv_feed_frames_at_level(TEST_LIGHT_RESUME_THRESHOLD - 1, 1);
  cl_assert_equal_i(s_encoded_frames, encoded_before);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);

  // Still below the QUIET threshold, so this frame is not "speech" by the entry rule -- and it
  // resumes anyway. That asymmetry is the whole design.
  cl_assert(TEST_LIGHT_RESUME_THRESHOLD < TEST_LIGHT_QUIET_THRESHOLD);
  prv_feed_frames_at_level(TEST_LIGHT_RESUME_THRESHOLD, 1);
  cl_assert_equal_i(s_encoded_frames, encoded_before + 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);

  // 1 arming frame + 50 + the frame below the resume threshold, and the gap claims exactly those.
  AudioCompanionStreamGapMsg gap;
  cl_assert(prv_last_gap_with_reason(AudioCompanionGapReasonSilenceSuppressed, &gap));
  cl_assert_equal_i(gap.missing_frame_count, 52);
  cl_assert_equal_i(prv_diag().suppressed_silence_frames, 52);
}

//! Why a mean over 20 ms and not a peak. Every frame here contains a 0.125 ms knock at amplitude
//! 800 -- a hundred times the quiet threshold -- and the frame average dilutes it to 7, so the
//! room reads as quiet. Swap this rule for anything peak-based and suppression stops firing on a
//! real wrist, which is precisely the failure being fixed.
void test_audio_companion__a_short_transient_inside_a_quiet_frame_is_not_speech(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);

  for (uint32_t i = 0; i < TEST_LIGHT_WINDOW_FRAMES; i++) {
    prv_feed_frame_with_transient(2, 800, 2);
  }
  fake_system_task_callbacks_invoke_pending();

  cl_assert(prv_diag().silence_level < TEST_LIGHT_QUIET_THRESHOLD);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);
}

//! What the detector must NOT measure. A steady offset is the shape wrist movement, sleeve
//! contact and body-conducted thump make: loud by any raw measure, entirely below the frequencies
//! a person speaks at, and discarded by Speex -- so no recording can ever show it and no threshold
//! derived from one would be meaningful. Simulation could not reconcile "never fires at threshold
//! 40" with the decoded audio by a factor of nine, and this is the leading explanation.
void test_audio_companion__the_detector_ignores_sub_audio_rumble(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);

  for (uint32_t i = 0; i < TEST_LIGHT_WINDOW_FRAMES; i++) {
    prv_feed_frame_with_sample(200);
  }
  fake_system_task_callbacks_invoke_pending();

  const AudioCompanionDiagnostics diag = prv_diag();
  cl_assert_equal_i(diag.silence_raw_level, 200);
  cl_assert(diag.silence_level < TEST_LIGHT_QUIET_THRESHOLD);
  cl_assert_equal_b(diag.silence_suppressing, true);
}

//! The high-pass has to DECAY, from either sign, and integer state is where that goes wrong.
//!
//! With the filter memory held as a plain int and a pole at 0.99, `(A * y) >> 15` is an arithmetic
//! shift: it floors, so y = -99 maps back to -99 and stays there. A single negative-going step --
//! an arm coming to rest on a desk -- would then pin the reported level at ~99, five times the
//! quiet threshold, for the rest of the session, and the detector would never fire again. That is
//! the same shape of failure this whole change exists to fix, one layer down.
void test_audio_companion__the_high_pass_decays_after_a_negative_step(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);

  for (uint32_t i = 0; i < 4; i++) {
    prv_feed_frame_with_sample(500);  // rumble on
  }
  for (uint32_t i = 0; i < 8; i++) {
    prv_feed_frame_with_sample(0);  // ...and off again
  }
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(prv_diag().silence_level, 0);

  // ...and the detector still arms afterwards, which is exactly what a latched filter prevented.
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);
}

//! The window is deliberately NOT emptied on resume -- a cough costs the frames it occupies plus
//! the re-arm hangover, not a fresh 20 s window. The hangover is what stops the rule collapsing
//! into a per-frame transmit gate that suppresses the pauses between words ~2,700 times an hour.
void test_audio_companion__re_arming_costs_the_hangover_and_not_a_whole_window(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);

  prv_feed_frames_at_level(900, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);

  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_REARM_FRAMES - 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);
  cl_assert_equal_i(prv_diag().silence_runs, 2);
}

//! Balanced is a different threshold AND a different window, and changing mode ends whatever run
//! was in progress. It used to end one only when switching to Off, which left a live run and a
//! half-filled window being judged by rules they were not collected under.
void test_audio_companion__changing_mode_ends_the_run_and_re_arms_from_scratch(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);

  s_data_count = 0;
  const uint32_t suppressed_by_light = prv_diag().suppressed_silence_frames;
  audio_companion_set_silence_mode(AudioCompanionSilenceModeBalanced);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(prv_diag().silence_suppressing, false);
  cl_assert_equal_i(prv_diag().silence_enter_threshold, TEST_BALANCED_QUIET_THRESHOLD);
  cl_assert_equal_i(prv_diag().silence_resume_threshold, TEST_BALANCED_RESUME_THRESHOLD);

  // 9 is speech to Light and quiet to Balanced, so this stretch also proves the new threshold is
  // in force rather than the old one, and that the window restarted rather than carrying over.
  cl_assert(TEST_LIGHT_QUIET_THRESHOLD <= 9 && 9 < TEST_BALANCED_QUIET_THRESHOLD);
  prv_feed_frames_at_level(9, TEST_BALANCED_WINDOW_FRAMES - 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);
  prv_feed_frames_at_level(9, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);

  // The Light run was closed honestly rather than merged into the Balanced one: the gap the mode
  // change produced covers exactly what Light skipped. (It reaches the phone on the next drain
  // tick, which the frames above provide.)
  AudioCompanionStreamGapMsg gap;
  cl_assert(prv_last_gap_with_reason(AudioCompanionGapReasonSilenceSuppressed, &gap));
  cl_assert_equal_i(gap.missing_frame_count, suppressed_by_light);
}

//! Aggressive is the short-window level, and its resume threshold sits closest to its quiet
//! threshold -- which is what buys the extra airtime and what costs the occasional 20 ms trim.
void test_audio_companion__aggressive_uses_a_short_window_and_a_higher_resume(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeAggressive);
  prv_feed_frames_at_level(10, TEST_AGGRESSIVE_WINDOW_FRAMES - 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);
  prv_feed_frames_at_level(10, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);

  // A level that would have resumed Light stays suppressed here.
  cl_assert(TEST_LIGHT_RESUME_THRESHOLD < TEST_AGGRESSIVE_RESUME_THRESHOLD);
  prv_feed_frames_at_level(TEST_AGGRESSIVE_RESUME_THRESHOLD - 1, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);

  const uint32_t encoded_before = s_encoded_frames;
  prv_feed_frames_at_level(TEST_AGGRESSIVE_RESUME_THRESHOLD, 1);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);
  cl_assert_equal_i(s_encoded_frames, encoded_before + 1);
}

//! The BLE connection interval is renegotiated on DURATION, not on the suppression flag.
//!
//! At the measured 60-200 suppression episodes an hour, doing it on every edge is 120-400
//! connection-parameter renegotiations an hour -- more radio time than the payload those short
//! episodes save. Short runs stay at the streaming interval; only runs worth relaxing for get it.
void test_audio_companion__short_quiet_runs_do_not_renegotiate_the_connection(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);

  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_BLE_RELAX_FRAMES - 2);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);

  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 1);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMax);

  // ...and speech puts it straight back.
  prv_feed_frames_at_level(900, 1);
  cl_assert_equal_i(s_response_time_state, ResponseTimeMiddle);
}

//! The stationary power-save mute reads the detector at the end of each listen window, so the
//! verdict has to be reachable inside that window. It was `s_silence_suppressing` against a
//! 5,000 ms window while the default mode needed 5,000 ms of quiet to become true, so it could
//! never be reached and the mute -- the ONLY thing that stops mic_start() -- could not re-engage
//! after its first listen.
//!
//! The fix is on the verdict rather than the window: arming windows are 5-20 s and stretching
//! every listen to fit the longest would leave the microphone on for a sixth of each cycle, which
//! is most of what the mute saves. The mute asks the narrower question over what it did hear.
void test_audio_companion__the_power_save_listen_window_can_reach_a_verdict(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);

  audio_companion_set_runlevel(RunLevel_Stationary);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
  cl_assert(!s_mic_running);

  const TimerID listen = audio_companion_test_get_power_save_listen_timer();
  cl_assert(listen != TIMER_INVALID_ID);
  cl_assert_equal_b(stub_new_timer_fire(listen), true);  // the window opens
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);

  const uint32_t window_frames =
      stub_new_timer_timeout(listen) / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS;
  cl_assert(window_frames > TEST_POWER_SAVE_LISTEN_MIN_FRAMES);

  // A quiet room for the whole window: the mute must re-engage when it closes.
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, window_frames);
  cl_assert_equal_b(stub_new_timer_fire(listen), true);  // the window closes
  fake_system_task_callbacks_invoke_pending();
  cl_assert(!s_mic_running);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);

  // ...and a window that hears speech keeps the microphone, which is the point of asking.
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);
  prv_feed_frames_at_level(900, window_frames);
  cl_assert_equal_b(stub_new_timer_fire(listen), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_mic_running);
}

//! Receiver traffic used to assign TIMER_INVALID_ID over the power-save and capture-retry handles
//! instead of stopping the timers. That forgets a timer rather than stopping it: the old one
//! stays scheduled and its slot in task_timer.c's fixed pool is never returned, while the next
//! arm allocates a fresh one. The phone talks to the watch every 0.5-2 s, so this ran constantly,
//! and exhausting that pool is a PBL_ASSERTN -- a watch reset with no obvious cause.
void test_audio_companion__receiver_traffic_does_not_leak_the_power_save_timer(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES);

  audio_companion_set_runlevel(RunLevel_Stationary);
  const TimerID listen = audio_companion_test_get_power_save_listen_timer();
  cl_assert(listen != TIMER_INVALID_ID);
  const int creates_before = s_num_new_timer_create_calls;

  for (int i = 0; i < 5; i++) {
    prv_send_receiver_health((uint8_t)(0x50 + i));
    fake_system_task_callbacks_invoke_pending();
    // The handle survives the keepalive, and so does the timer behind it.
    cl_assert_equal_i(audio_companion_test_get_power_save_listen_timer(), listen);
    cl_assert_equal_b(stub_new_timer_is_scheduled(listen), true);
    audio_companion_set_runlevel(RunLevel_Normal);
    audio_companion_set_runlevel(RunLevel_Stationary);
  }
  cl_assert_equal_i(s_num_new_timer_create_calls, creates_before);
}

//! The counter existed for three months and was never displayed, so "it has never fired" was
//! indistinguishable from "nobody looked". Diagnostics now carry the whole picture: how much was
//! skipped, how many runs, the live level with and without the high-pass, both thresholds it is
//! judged against, and how full the window is.
void test_audio_companion__diagnostics_report_what_the_detector_is_doing(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);

  prv_feed_frames_at_level(120, 10);
  AudioCompanionDiagnostics diag = prv_diag();
  cl_assert_equal_i(diag.silence_level, 120);
  cl_assert_equal_i(diag.silence_enter_threshold, TEST_LIGHT_QUIET_THRESHOLD);
  cl_assert_equal_i(diag.silence_resume_threshold, TEST_LIGHT_RESUME_THRESHOLD);
  cl_assert_equal_i(diag.silence_quiet_permille, 0);
  cl_assert_equal_i(diag.suppressed_silence_frames, 0);
  cl_assert_equal_b(diag.silence_suppressing, false);

  // Ten loud frames then ninety quiet ones: 90% of what has been seen so far.
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 90);
  diag = prv_diag();
  cl_assert_equal_i(diag.silence_level, TEST_QUIET_LEVEL);
  cl_assert_equal_i(diag.silence_quiet_permille, 900);

  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES);
  diag = prv_diag();
  cl_assert_equal_b(diag.silence_suppressing, true);
  cl_assert(diag.suppressed_silence_frames > 0);
  cl_assert_equal_i(diag.silence_runs, 1);

  // Off means off: no thresholds to report and no detector running.
  audio_companion_set_silence_mode(AudioCompanionSilenceModeOff);
  fake_system_task_callbacks_invoke_pending();
  diag = prv_diag();
  cl_assert_equal_i(diag.silence_enter_threshold, 0);
  cl_assert_equal_i(diag.silence_resume_threshold, 0);
  cl_assert_equal_b(diag.silence_suppressing, false);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES + 10);
  cl_assert_equal_b(prv_diag().silence_suppressing, false);
}

//! Suppression saves the encoder and the radio; it does NOT release the microphone's PDM rails,
//! buffers and clocks, which the Session 17 battery audit found to be the dominant cost. The
//! offline park used to be driven only from the encoded path, so a quiet room plus a receiver
//! that was never coming back would have held the microphone open until the battery was flat --
//! a hazard that could not happen while the detector was unable to fire, and can now.
void test_audio_companion__quiet_does_not_keep_the_mic_alive_for_a_receiver_that_is_gone(void) {
  prv_start_capture_with_mode(AudioCompanionSilenceModeLight);
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, TEST_LIGHT_WINDOW_FRAMES + 5);
  cl_assert_equal_b(prv_diag().silence_suppressing, true);
  cl_assert(s_mic_running);

  // The probe goes out during the quiet and nobody answers, so the receiver really is gone.
  const TimerID probe = prv_drain_until_silence_probe_armed();
  cl_assert(probe != TIMER_INVALID_ID);
  cl_assert_equal_b(stub_new_timer_fire(probe), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(stub_new_timer_fire(probe), true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateAuthorizedIdle);
  // Still capturing on purpose: an unanswered probe is usually an iOS app that iOS suspended.
  cl_assert(s_mic_running);

  // The room stays quiet, so nothing is ever encoded and the spool never saturates. Time is the
  // only thing left that can stop the microphone.
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 10);
  cl_assert(s_mic_running);
  s_uptime_seconds += (10 * 60) + 1;
  prv_feed_frames_at_level(TEST_QUIET_LEVEL, 10);
  cl_assert(!s_mic_running);
}

// A receiver that (re)authorizes while capture is paused must still be told about the stream
// before it is handed a single frame or gap record. The re-announcement used to be requested
// only from the streaming branch of the state machine, so a session that attached during a
// power-save pause left with `need_stream_start` clear -- and the next drain, whenever it came,
// sent the spool's frames and pending gaps for a stream the receiver had never heard of. On
// 2026-09-09 that was a fresh phone process re-authorizing on the same BLE connection into a
// stationary mute: 686 frames with nowhere to go, six gap notices dropped, and a full link
// rebuild to recover.
void test_audio_companion__reauth_during_a_pause_reannounces_before_any_data(void) {
  audio_companion_set_enabled(true);
  prv_subscribe(true, true);
  prv_authenticate();
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  cl_assert(!audio_companion_test_stream_reannounce_pending());

  // Capture pauses for power save; the stream itself stays active.
  audio_companion_set_runlevel(RunLevel_LowPower);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);
  cl_assert(!s_mic_running);

  // The receiver process dies and a new one attaches on the same connection: it re-subscribes
  // and re-authorizes while the watch is still paused.
  prv_subscribe(false, false);
  prv_subscribe(true, true);
  prv_authenticate();
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStatePausedPowerSave);

  // The fresh session's first data message is now the stream's re-announcement.
  cl_assert(audio_companion_test_stream_reannounce_pending());

  // ...and when capture resumes, that is exactly what it hears first, flagged as a RESUME.
  s_data_count = 0;
  audio_companion_set_runlevel(RunLevel_Normal);
  cl_assert_equal_i(audio_companion_get_state(), AudioCompanionServiceStateStreaming);
  prv_feed_frames(TEST_DRAIN_PUSH_FRAMES);
  cl_assert(s_data_count > 0);
  cl_assert_equal_i(s_data_notifications[0].data[0], AudioCompanionDataMsgIdStreamStart);
  AudioCompanionStreamStartMsg start;
  memcpy(&start, s_data_notifications[0].data, sizeof(start));
  cl_assert_equal_i(start.flags & AUDIO_COMPANION_STREAM_START_FLAG_RESUME,
                    AUDIO_COMPANION_STREAM_START_FLAG_RESUME);
  cl_assert(!audio_companion_test_stream_reannounce_pending());
}
