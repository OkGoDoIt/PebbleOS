/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/audio_companion.h"

#include "auth.h"
#include "reboot_trace.h"
#include "spool.h"

#include "applib/event_service_client.h"
#include "bluetooth/audio_companion_service.h"
#include "comm/bt_conn_mgr.h"
#include "board/board.h"
#include <pbl/drivers/mic.h>
#include <pbl/drivers/rtc.h>
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "pbl/os/mutex.h"
#include "pbl/services/battery/battery_state.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/runlevel.h"
#include "pbl/services/system_task.h"
#include "pbl/services/voice/voice_speex.h"
#include "shell/prefs.h"
#include "pbl/logging/logging.h"
#include "system/passert.h"
#include "system/reboot_reason.h"
#include "util/rand.h"
#include "util/time/time.h"

#ifndef UNITTEST
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
#endif

#include <inttypes.h>
#include <string.h>

#ifndef CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT
#define CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT 20
#endif

#define DRAIN_PERIOD_MS (150)
#define DRAIN_PUSH_THRESHOLD_FRAMES (8)
//! Cap the BLE notifications sent in a single drain callback so the callback returns promptly
//! (keeping the KernelBG watchdog fed) and yields the system task to other work even when a large
//! post-reconnect backlog needs to flush. Any remainder is drained on a freshly posted callback.
#define DRAIN_MAX_BATCHES_PER_CALL (16)
//! Stop capturing if a streaming session goes this long with no control message from the
//! receiver. The phone checkpoints every ~0.5-2 s while receiving, so a long silence means the
//! app is gone even when the watch never saw a BLE disconnect (e.g. the receiver shares the link
//! with the official app and only dropped its own GATT connection).
#define RECEIVER_LIVENESS_TIMEOUT_MS (15 * 1000)
#define CONSENT_TIMEOUT_MS (AUDIO_COMPANION_CONSENT_TIMEOUT_SECONDS * 1000)
#define LOW_BATTERY_RESUME_HYSTERESIS_PCT (5)
//! Alert when this many frames have been lost since the last alert (30 s of audio).
#define LOSS_ALERT_THRESHOLD_FRAMES (30 * 1000 / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS)
#define LOSS_ALERT_MIN_INTERVAL_SECONDS (6 * 60 * 60)
//! Lightweight silence suppression uses mean absolute PCM level before Speex's gain stage.
//! Thresholds are intentionally low because dropping quiet speech is worse than sending noise.
#define SILENCE_MODE_LIGHT_ENTER_THRESHOLD (40)
#define SILENCE_MODE_LIGHT_EXIT_THRESHOLD (64)
#define SILENCE_MODE_LIGHT_ENTER_MS (5000)
#define SILENCE_MODE_LIGHT_WEAK_EXIT_FRAMES (1)
#define SILENCE_MODE_BALANCED_ENTER_THRESHOLD (64)
#define SILENCE_MODE_BALANCED_EXIT_THRESHOLD (96)
#define SILENCE_MODE_BALANCED_ENTER_MS (3500)
#define SILENCE_MODE_BALANCED_WEAK_EXIT_FRAMES (3)
#define SILENCE_MODE_AGGRESSIVE_ENTER_THRESHOLD (95)
#define SILENCE_MODE_AGGRESSIVE_EXIT_THRESHOLD (150)
#define SILENCE_MODE_AGGRESSIVE_ENTER_MS (2000)
#define SILENCE_MODE_AGGRESSIVE_WEAK_EXIT_FRAMES (5)

typedef struct {
  bool data_subscribed;
  bool control_subscribed;
  bool authorized;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
  uint8_t granted_proto_version;
} ReceiverSession;

typedef struct {
  bool pending;
  uint8_t request_token;
  uint8_t proto_version;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
  char name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];
} ConsentRequest;

typedef struct {
  bool pending;
  uint8_t request_token;
} EnableRequest;

typedef struct {
  uint32_t enter_threshold;
  uint32_t exit_threshold;
  uint32_t enter_frames;
  uint32_t weak_exit_frames;
} SilenceModeConfig;

static PebbleMutex *s_lock;
static bool s_initialized;
static AudioCompanionServiceState s_state = AudioCompanionServiceStateDisabled;
static bool s_enabled;
static ReceiverSession s_session;
static ConsentRequest s_consent;
static EnableRequest s_enable_request;
static AudioCompanionConsentHandler s_consent_handler;
static AudioCompanionEnableHandler s_enable_handler;
static TimerID s_consent_timer = TIMER_INVALID_ID;
static TimerID s_enable_timer = TIMER_INVALID_ID;

// Stream + capture state
//!
//! Capture intent vs. reality. The mic driver calls prv_mic_data_handler() with its own mutex
//! held, and that handler takes s_lock -- so on the capture path the driver mutex is always
//! acquired *before* s_lock. Calling mic_start()/mic_stop() while holding s_lock takes the same
//! two locks in the opposite order, so any task that did that (dictation from the App task,
//! runlevel/battery changes from KernelMain, the settings UI) could deadlock against KernelBG
//! sitting mid-dispatch. KernelBG then never feeds the task watchdog again and the watch resets
//! ~7 s later with RebootReasonCode_Watchdog.
//!
//! So the state machine only records *intent* (s_owns_mic / s_capture_wanted) under s_lock, and
//! prv_apply_capture() performs the driver calls with s_lock dropped.
static bool s_owns_mic;                //!< service intends to hold the mic (guarded by s_lock)
static bool s_capture_wanted;          //!< intent handed to prv_apply_capture() (guarded by s_lock)
static bool s_capture_start_failed;    //!< last apply could not take the mic (guarded by s_lock)
static bool s_mic_started;             //!< driver really started; owned by prv_apply_capture()
static PebbleMutex *s_capture_lock;    //!< serializes appliers; never held while taking s_lock
static bool s_stream_active;
static bool s_need_stream_start;
static bool s_stream_resumed;    //!< the pending STREAM_START re-announces an ongoing stream
static bool s_stream_attached;   //!< current ready session has been (re)announced the stream
static uint32_t s_stream_id;
static uint32_t s_next_sequence;
static uint64_t s_next_sample_index;
static TimerID s_drain_timer = TIMER_INVALID_ID;
static TimerID s_catch_up_timer = TIMER_INVALID_ID;
static bool s_catch_up_burst;
static bool s_drain_cb_pending;        //!< a drain callback is queued; coalesces drain posts
static bool s_capture_parked;          //!< capture stopped after offline overflow
static uint32_t s_pause_started_ms;    //!< uptime when a gap-producing pause began
static uint8_t s_pending_resume_gap_reason;
static uint32_t s_last_receiver_activity_ms;  //!< uptime of the last control message received
static bool s_receiver_presumed_gone;  //!< liveness watchdog tripped; treat session as not ready

// Policy inputs
static bool s_mic_conflict_active;
static bool s_receiver_pause_requested;
static bool s_low_battery;
static bool s_pause_stationary_enabled;
static bool s_pause_low_power_enabled;
static AudioCompanionSilenceMode s_silence_mode = AudioCompanionSilenceModeLight;
static RunLevel s_runlevel = RunLevel_Normal;
static bool s_error;

// Diagnostics
static uint32_t s_captured_frames;
static uint32_t s_sent_frames;
static uint32_t s_send_backpressure_events;
static uint32_t s_mic_conflicts;
static uint32_t s_suppressed_silence_frames;
static uint32_t s_loss_alerts_posted;
static uint32_t s_alert_baseline_dropped;
static uint32_t s_last_alert_uptime_s;
static uint32_t s_offline_baseline_dropped;
static uint32_t s_silence_candidate_frames;
static uint32_t s_silence_resume_candidate_frames;
static bool s_silence_suppressing;
static uint32_t s_silence_gap_frames;
static uint32_t s_silence_gap_first_sequence;
static uint64_t s_silence_gap_first_sample_index;

static EventServiceInfo s_battery_event_info;

// Reboot flight recorder (cached copy of the persisted ring; populated once at boot).
static AudioCompanionRebootTrace s_reboot_trace;
static bool s_reboot_trace_recorded;

static void prv_reevaluate_locked(void);
static void prv_drain_system_task_cb(void *data);
static void prv_post_drain_cb(void);
static void prv_start_drain_timer_locked(void);
static void prv_stop_drain_timer_locked(void);
static void prv_update_ble_responsiveness_locked(void);

// ---- Small helpers ----

static uint32_t prv_uptime_ms(void) { return time_get_uptime_seconds() * 1000; }

static uint64_t prv_wall_clock_ms(void) {
  time_t seconds = 0;
  uint16_t ms = 0;
  rtc_get_time_ms(&seconds, &ms);
  return (uint64_t)seconds * 1000 + ms;
}

static bool prv_session_ready_locked(void) {
  return s_session.authorized && s_session.data_subscribed && !s_receiver_presumed_gone;
}

//! Background capture only runs at RunLevel_Normal. Stationary and LowPower pause unconditionally
//! (deliberate: the pause is not user-overridable), and BareMinimum / FirmwareUpdate must stop the
//! mic too -- those runlevels are entered for panics, factory resets and firmware updates, where
//! holding the mic and the Speex encoder open is exactly the wrong thing to do.
static bool prv_power_save_active_locked(void) {
  return s_runlevel != RunLevel_Normal;
}

static const SilenceModeConfig *prv_silence_mode_config(AudioCompanionSilenceMode mode) {
  static const SilenceModeConfig s_configs[] = {
    [AudioCompanionSilenceModeLight] = {
      .enter_threshold = SILENCE_MODE_LIGHT_ENTER_THRESHOLD,
      .exit_threshold = SILENCE_MODE_LIGHT_EXIT_THRESHOLD,
      .enter_frames =
          SILENCE_MODE_LIGHT_ENTER_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
      .weak_exit_frames = SILENCE_MODE_LIGHT_WEAK_EXIT_FRAMES,
    },
    [AudioCompanionSilenceModeBalanced] = {
      .enter_threshold = SILENCE_MODE_BALANCED_ENTER_THRESHOLD,
      .exit_threshold = SILENCE_MODE_BALANCED_EXIT_THRESHOLD,
      .enter_frames =
          SILENCE_MODE_BALANCED_ENTER_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
      .weak_exit_frames = SILENCE_MODE_BALANCED_WEAK_EXIT_FRAMES,
    },
    [AudioCompanionSilenceModeAggressive] = {
      .enter_threshold = SILENCE_MODE_AGGRESSIVE_ENTER_THRESHOLD,
      .exit_threshold = SILENCE_MODE_AGGRESSIVE_EXIT_THRESHOLD,
      .enter_frames =
          SILENCE_MODE_AGGRESSIVE_ENTER_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
      .weak_exit_frames = SILENCE_MODE_AGGRESSIVE_WEAK_EXIT_FRAMES,
    },
  };
  if (mode <= AudioCompanionSilenceModeOff || mode >= AudioCompanionSilenceModeCount) {
    return NULL;
  }
  return &s_configs[mode];
}

static void prv_reset_silence_suppression_locked(void) {
  const bool was_suppressing = s_silence_suppressing;
  s_silence_candidate_frames = 0;
  s_silence_resume_candidate_frames = 0;
  s_silence_suppressing = false;
  s_silence_gap_frames = 0;
  s_silence_gap_first_sequence = 0;
  s_silence_gap_first_sample_index = 0;
  if (was_suppressing) {
    prv_update_ble_responsiveness_locked();
  }
}

static void prv_record_silence_gap_locked(void) {
  if (!s_silence_suppressing || s_silence_gap_frames == 0) {
    prv_reset_silence_suppression_locked();
    return;
  }
  audio_companion_spool_record_gap(s_silence_gap_first_sequence, s_silence_gap_frames,
                                   s_silence_gap_first_sample_index,
                                   AudioCompanionGapReasonSilenceSuppressed);
  // Suppressed silence intentionally has no receiver traffic. Re-arm liveness when traffic
  // resumes so the receiver gets a fresh window to persist and checkpoint the next notification.
  s_last_receiver_activity_ms = prv_uptime_ms();
  prv_reset_silence_suppression_locked();
}

static uint32_t prv_mean_abs_pcm(const int16_t *samples, size_t sample_count) {
  uint32_t sum = 0;
  for (size_t i = 0; i < sample_count; i++) {
    const int32_t sample = samples[i];
    sum += (sample < 0) ? (uint32_t)-sample : (uint32_t)sample;
  }
  return sample_count ? (sum / sample_count) : 0;
}

static bool prv_maybe_suppress_silence_locked(const int16_t *samples, size_t sample_count,
                                              bool *out_schedule_drain) {
  const SilenceModeConfig *config = prv_silence_mode_config(s_silence_mode);
  if (!config) {
    if (s_silence_suppressing && out_schedule_drain) {
      *out_schedule_drain = true;
    }
    prv_record_silence_gap_locked();
    return false;
  }

  const uint32_t mean_abs = prv_mean_abs_pcm(samples, sample_count);
  if (s_silence_suppressing) {
    const bool strong_resume = mean_abs >= config->exit_threshold;
    const bool weak_resume = mean_abs >= config->enter_threshold &&
        ++s_silence_resume_candidate_frames >= config->weak_exit_frames;
    if (strong_resume || weak_resume) {
      prv_record_silence_gap_locked();
      if (out_schedule_drain) {
        *out_schedule_drain = true;
      }
      return false;
    }
    if (mean_abs < config->enter_threshold) {
      s_silence_resume_candidate_frames = 0;
    }
  } else if (mean_abs < config->enter_threshold) {
    s_silence_candidate_frames++;
    if (s_silence_candidate_frames <= config->enter_frames) {
      return false;
    }
    s_silence_suppressing = true;
    s_silence_gap_frames = 0;
    s_silence_gap_first_sequence = s_next_sequence;
    s_silence_gap_first_sample_index = s_next_sample_index;
    prv_update_ble_responsiveness_locked();
  } else {
    s_silence_candidate_frames = 0;
    return false;
  }

  s_next_sequence++;
  s_next_sample_index += sample_count;
  s_captured_frames++;
  s_suppressed_silence_frames++;
  s_silence_gap_frames++;
  return true;
}

//! Mark the receiver as alive: called whenever a control message or a fresh subscription
//! arrives. Clears a tripped liveness watchdog.
static void prv_note_receiver_activity_locked(void) {
  s_last_receiver_activity_ms = prv_uptime_ms();
  s_receiver_presumed_gone = false;
}

static void prv_notify_control_locked(const uint8_t *data, size_t length) {
  if (!s_session.control_subscribed) {
    return;
  }
  bt_driver_audio_companion_notify_control(data, length);
}

static void prv_send_state_changed_locked(void) {
  uint8_t buf[sizeof(AudioCompanionStateChangedMsg)];
  const size_t len =
      audio_companion_protocol_build_state_changed(buf, sizeof(buf), (uint8_t)s_state);
  prv_notify_control_locked(buf, len);
}

static void prv_cancel_catch_up_burst_locked(void) {
  if (s_catch_up_burst) {
    s_catch_up_burst = false;
    if (s_catch_up_timer != TIMER_INVALID_ID) {
      new_timer_stop(s_catch_up_timer);
    }
  }
}

static void prv_update_ble_responsiveness_locked(void) {
  if (s_state == AudioCompanionServiceStateStreaming && s_catch_up_burst) {
    bt_driver_audio_companion_set_response_time(ResponseTimeMin,
                                                MIN_LATENCY_MODE_TIMEOUT_AUDIO_SECS);
  } else if (s_state == AudioCompanionServiceStateStreaming && !s_silence_suppressing) {
    bt_driver_audio_companion_set_response_time(ResponseTimeMiddle, MAX_PERIOD_RUN_FOREVER);
  } else {
    bt_driver_audio_companion_set_response_time(ResponseTimeMax, 0);
  }
}

static void prv_end_catch_up_burst_system_task_cb(void *data) {
  mutex_lock(s_lock);
  s_catch_up_burst = false;
  if (s_catch_up_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_catch_up_timer);
  }
  prv_update_ble_responsiveness_locked();
  mutex_unlock(s_lock);
}

static void prv_catch_up_timer_cb(void *data) {
  system_task_add_callback(prv_end_catch_up_burst_system_task_cb, NULL);
}

static void prv_begin_catch_up_burst_locked(void) {
  if (s_catch_up_burst) {
    return;
  }
  s_catch_up_burst = true;
  prv_update_ble_responsiveness_locked();
  if (s_catch_up_timer == TIMER_INVALID_ID) {
    s_catch_up_timer = new_timer_create();
  }
  new_timer_start(s_catch_up_timer, MIN_LATENCY_MODE_TIMEOUT_AUDIO_SECS * 1000,
                  prv_catch_up_timer_cb, NULL, 0);
}

static void prv_set_state_locked(AudioCompanionServiceState state) {
  if (s_state == state) {
    return;
  }
  if (state != AudioCompanionServiceStateStreaming) {
    prv_cancel_catch_up_burst_locked();
  }
  PBL_LOG_DBG("Audio companion state %u -> %u", (unsigned)s_state, (unsigned)state);
  s_state = state;
  prv_update_ble_responsiveness_locked();
  prv_send_state_changed_locked();
}

static void prv_send_auth_result_locked(uint8_t token, uint8_t status, uint8_t granted) {
  uint8_t buf[sizeof(AudioCompanionAuthResultMsg)];
  const size_t len =
      audio_companion_protocol_build_auth_result(buf, sizeof(buf), token, status, granted);
  prv_notify_control_locked(buf, len);
}

static void prv_send_ack_locked(uint8_t token, uint8_t status) {
  uint8_t buf[sizeof(AudioCompanionAckMsg)];
  const size_t len = audio_companion_protocol_build_ack(buf, sizeof(buf), token, status);
  prv_notify_control_locked(buf, len);
}

static void prv_send_error_locked(uint8_t error_code, uint32_t detail) {
  uint8_t buf[sizeof(AudioCompanionErrorMsg)];
  const size_t len =
      audio_companion_protocol_build_error(buf, sizeof(buf), error_code, detail);
  prv_notify_control_locked(buf, len);
}

static void prv_send_revoked_locked(uint8_t reason) {
  uint8_t buf[sizeof(AudioCompanionRevokedMsg)];
  const size_t len = audio_companion_protocol_build_revoked(buf, sizeof(buf), reason);
  prv_notify_control_locked(buf, len);
}

// ---- Loss alert ----

static void prv_post_loss_alert(void) {
#ifndef UNITTEST
  AttributeList attr_list = {0};
  attribute_list_add_cstring(&attr_list, AttributeIdTitle, "Audio Companion");
  attribute_list_add_cstring(&attr_list, AttributeIdBody,
                             "Some audio could not be delivered to your audio app and was "
                             "skipped.");
  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0, TimelineItemTypeNotification, LayoutIdNotification, &attr_list, NULL);
  attribute_list_destroy_list(&attr_list);
  if (item) {
    item->header.from_watch = true;
    notifications_add_notification(item);
    timeline_item_destroy(item);
  }
#endif
  s_loss_alerts_posted++;
}

static void prv_loss_alert_system_task_cb(void *data) {
  // Building a timeline notification allocates and touches the notification store; keep it off the
  // mic capture path (which runs under both s_lock and the mic driver's mutex on KernelBG).
  prv_post_loss_alert();
}

//! Decides (under s_lock) whether a "some audio was skipped" alert is due. Returns true if the
//! caller should post it from a plain system-task callback; the heavy notification work must not
//! run inline on the locked mic path.
static bool prv_maybe_alert_loss_locked(void) {
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  if (stats.dropped_overflow_frames < s_alert_baseline_dropped + LOSS_ALERT_THRESHOLD_FRAMES) {
    return false;
  }
  const uint32_t now_s = time_get_uptime_seconds();
  s_alert_baseline_dropped = stats.dropped_overflow_frames;
  if (s_last_alert_uptime_s != 0 &&
      (now_s - s_last_alert_uptime_s) < LOSS_ALERT_MIN_INTERVAL_SECONDS) {
    return false;
  }
  s_last_alert_uptime_s = now_s;
  PBL_LOG_WRN("Audio companion lost >%u frames; alerting user",
              (unsigned)LOSS_ALERT_THRESHOLD_FRAMES);
  return true;
}

// ---- Capture ----

static void prv_mic_data_handler(int16_t *samples, size_t sample_count, void *context);

//! Record that capture should be running. The mic driver is only touched later, by
//! prv_apply_capture(), which the caller must invoke once it has dropped s_lock.
static void prv_start_capture_locked(void) {
  s_capture_wanted = true;
  s_owns_mic = true;
  s_capture_parked = false;
}

//! Record that capture should stop, and clear the "mic was unavailable" latch so a later attempt
//! is allowed. Like prv_start_capture_locked(), the driver call happens in prv_apply_capture().
static void prv_stop_capture_locked(void) {
  s_capture_wanted = false;
  s_owns_mic = false;
  s_capture_start_failed = false;
}

//! Reconcile the mic driver with the intent recorded under s_lock.
//!
//! MUST be called with s_lock released: this is the function that actually takes the driver's
//! mutex, and taking it under s_lock is the lock-order inversion described above. s_capture_lock
//! serializes concurrent appliers and is itself never acquired while s_lock is held, so the
//! global order stays acyclic: s_capture_lock -> mic mutex -> s_lock.
static void prv_apply_capture(void) {
  if (!s_capture_lock) {
    return;
  }
  mutex_lock(s_capture_lock);
  for (;;) {
    mutex_lock(s_lock);
    const bool want = s_capture_wanted;
    mutex_unlock(s_lock);

    if (want == s_mic_started) {
      break;
    }

    if (!want) {
      if (mic_is_running(MIC)) {
        mic_stop(MIC);
      }
      if (voice_speex_is_initialized()) {
        voice_speex_deinit();
      }
      s_mic_started = false;
      PBL_LOG_INFO("Audio companion: capture stopped");
      continue;  // intent may have flipped again while the driver call ran
    }

    bool started = false;
    if (voice_speex_init()) {
      int16_t *frame_buffer = voice_speex_get_frame_buffer();
      const size_t frame_samples = voice_speex_get_frame_size();
      if (frame_buffer && frame_samples > 0) {
        started = mic_start(MIC, prv_mic_data_handler, NULL, frame_buffer, frame_samples);
      }
      if (!started) {
        voice_speex_deinit();
      }
    } else {
      PBL_LOG_ERR("Audio companion: speex init failed");
    }
    s_mic_started = started;

    if (started) {
      PBL_LOG_INFO("Audio companion: capture started");
      continue;
    }

    // Mic already owned by someone else (e.g. dictation) or out of memory. Latch the failure so
    // the state machine settles on PausedConflict instead of spinning on a start we cannot
    // complete; audio_companion_mic_conflict_end() (or any pause) clears the latch.
    PBL_LOG_WRN("Audio companion: mic unavailable; treating as conflict");
    mutex_lock(s_lock);
    s_capture_wanted = false;
    s_owns_mic = false;
    s_capture_start_failed = true;
    prv_reevaluate_locked();
    mutex_unlock(s_lock);
  }
  mutex_unlock(s_capture_lock);
}

//! Begin a pause that will become a STREAM_GAP when capture resumes.
static void prv_begin_gap_pause_locked(uint8_t gap_reason) {
  if (s_pending_resume_gap_reason == 0) {
    s_pause_started_ms = prv_uptime_ms();
    s_pending_resume_gap_reason = gap_reason;
  }
}

//! Record the gap for an elapsed pause; consumes sequence numbers so gap
//! ranges and subsequent data stay self-consistent.
static void prv_finish_gap_pause_locked(void) {
  if (s_pending_resume_gap_reason == 0 || !s_stream_active) {
    s_pending_resume_gap_reason = 0;
    return;
  }
  const uint32_t elapsed_ms = prv_uptime_ms() - s_pause_started_ms;
  uint32_t missing = elapsed_ms / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS;
  if (missing == 0) {
    missing = 1;
  }
  audio_companion_spool_record_gap(s_next_sequence, missing, s_next_sample_index,
                                   s_pending_resume_gap_reason);
  s_next_sequence += missing;
  s_next_sample_index += (uint64_t)missing * AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES;
  s_pending_resume_gap_reason = 0;
}

static void prv_park_capture_system_task_cb(void *data) {
  mutex_lock(s_lock);
  if (s_owns_mic && !prv_session_ready_locked()) {
    PBL_LOG_DBG("Audio companion: spool saturated while offline; parking capture");
    prv_stop_capture_locked();
    s_capture_parked = true;
    prv_begin_gap_pause_locked(AudioCompanionGapReasonTransportReset);
  }
  mutex_unlock(s_lock);
  prv_apply_capture();
}

static void prv_mic_data_handler(int16_t *samples, size_t sample_count, void *context) {
  mutex_lock(s_lock);
  if (!s_owns_mic || !s_stream_active) {
    mutex_unlock(s_lock);
    return;
  }
  const size_t expected_samples = (size_t)voice_speex_get_frame_size();
  if (sample_count != expected_samples) {
    mutex_unlock(s_lock);
    return;
  }

  bool schedule_drain = false;
  if (prv_maybe_suppress_silence_locked(samples, sample_count, &schedule_drain)) {
    mutex_unlock(s_lock);
    if (schedule_drain) {
      prv_post_drain_cb();
    }
    return;
  }

  uint8_t encoded[AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES];
  const int encoded_bytes = voice_speex_encode_frame(samples, encoded, sizeof(encoded));
  if (encoded_bytes <= 0) {
    mutex_unlock(s_lock);
    return;
  }

  const uint32_t sequence = s_next_sequence++;
  const uint64_t sample_index = s_next_sample_index;
  s_next_sample_index += expected_samples;
  s_captured_frames++;
  audio_companion_spool_push(sequence, sample_index, encoded, (uint16_t)encoded_bytes);

  bool schedule_park = false;
  if (prv_session_ready_locked()) {
    // Keep any drain already requested by silence suppression: exiting suppression must flush the
    // recorded gap (and the resuming frame) promptly. The drain timer is stopped while suppressing,
    // so overwriting this would strand the gap until enough frames re-accumulate.
    schedule_drain = schedule_drain ||
        (audio_companion_spool_frames_pending_send() >= DRAIN_PUSH_THRESHOLD_FRAMES);
  } else {
    // Offline: buffer until the spool starts dropping, then park the mic.
    AudioCompanionSpoolStats stats;
    audio_companion_spool_get_stats(&stats);
    schedule_park = stats.dropped_overflow_frames > s_offline_baseline_dropped;
  }
  if (schedule_drain && prv_session_ready_locked()) {
    prv_start_drain_timer_locked();
  }
  const bool post_loss_alert = prv_maybe_alert_loss_locked();
  mutex_unlock(s_lock);

  if (schedule_drain) {
    prv_post_drain_cb();
  }
  if (schedule_park) {
    system_task_add_callback(prv_park_capture_system_task_cb, NULL);
  }
  if (post_loss_alert) {
    system_task_add_callback(prv_loss_alert_system_task_cb, NULL);
  }
}

// ---- Stream / drain ----

static void prv_send_stream_start_locked(void) {
  const AudioCompanionStreamStartMsg params = {
    .protocol_version = s_session.granted_proto_version,
    .stream_id = s_stream_id,
    .codec_id = AudioCompanionCodecSpeexWideband,
    .channels = 1,
    .frame_samples = AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES,
    .sample_rate_hz = AUDIO_COMPANION_DEFAULT_SAMPLE_RATE_HZ,
    .bit_rate_bps = AUDIO_COMPANION_DEFAULT_BIT_RATE_BPS,
    .frame_duration_ms = AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
    .start_time_ms = prv_wall_clock_ms(),
    .start_monotonic_ms = prv_uptime_ms(),
    .flags = s_stream_resumed ? AUDIO_COMPANION_STREAM_START_FLAG_RESUME : 0,
  };
  uint8_t buf[sizeof(AudioCompanionStreamStartMsg)];
  const size_t len = audio_companion_protocol_build_stream_start(buf, sizeof(buf), &params);
  if (bt_driver_audio_companion_notify_data(buf, len)) {
    PBL_LOG_INFO("Audio companion stream start sent (stream_id=%" PRIu32 ")", s_stream_id);
    s_need_stream_start = false;
  } else {
    s_send_backpressure_events++;
  }
}

static void prv_send_stream_stop_locked(uint8_t reason) {
  if (!s_stream_active) {
    return;
  }
  const AudioCompanionStreamStopMsg params = {
    .stream_id = s_stream_id,
    .reason = reason,
    .final_sequence = (s_next_sequence > 0) ? (s_next_sequence - 1) : 0,
    .final_sample_index = s_next_sample_index,
    .counters_crc_or_zero = 0,
  };
  uint8_t buf[sizeof(AudioCompanionStreamStopMsg)];
  const size_t len = audio_companion_protocol_build_stream_stop(buf, sizeof(buf), &params);
  if (prv_session_ready_locked()) {
    bt_driver_audio_companion_notify_data(buf, len);
  }
}

static bool prv_send_pending_gap_locked(void) {
  AudioCompanionSpoolPendingGap gap;
  if (!audio_companion_spool_has_pending_gap()) {
    return true;
  }
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  if (!audio_companion_spool_take_pending_gap(&gap)) {
    return true;
  }
  const AudioCompanionStreamGapMsg params = {
    .stream_id = s_stream_id,
    .first_missing_sequence = gap.first_missing_sequence,
    .missing_frame_count = gap.missing_frame_count,
    .first_missing_sample_index = gap.first_missing_sample_index,
    .reason = gap.reason,
    .watch_drop_counter = stats.dropped_overflow_frames,
  };
  uint8_t buf[sizeof(AudioCompanionStreamGapMsg)];
  const size_t len = audio_companion_protocol_build_stream_gap(buf, sizeof(buf), &params);
  if (!bt_driver_audio_companion_notify_data(buf, len)) {
    // Put it back so it is not lost; resend on the next drain.
    audio_companion_spool_record_gap(gap.first_missing_sequence, gap.missing_frame_count,
                                     gap.first_missing_sample_index, gap.reason);
    s_send_backpressure_events++;
    return false;
  }
  return true;
}

static bool prv_send_data_batch_locked(void) {
  const uint16_t mtu = bt_driver_audio_companion_get_effective_mtu();
  if (mtu < sizeof(AudioCompanionStreamDataHeader) + 8) {
    return false;
  }
  const size_t max_message_bytes = (size_t)mtu - 3;

  // One stack buffer, not two: peek frames straight into the space after the header so this
  // runs in ~512 B on the 4 KB KernelBG stack (a second staging buffer + the NimBLE notify call
  // chain made the old frame uncomfortably deep).
  uint8_t message[512];
  const size_t header_size = sizeof(AudioCompanionStreamDataHeader);
  uint32_t first_sequence = 0;
  uint64_t first_sample_index = 0;
  uint8_t frame_count = 0;
  size_t payload_len = 0;

  const size_t budget =
      (max_message_bytes < sizeof(message)) ? max_message_bytes : sizeof(message);
  if (!audio_companion_spool_peek_batch(budget, header_size, &first_sequence,
                                        &first_sample_index, &frame_count, message + header_size,
                                        sizeof(message) - header_size, &payload_len)) {
    return false;
  }

  const size_t header_len = audio_companion_protocol_build_stream_data_header(
      message, header_size, s_stream_id, first_sequence, first_sample_index, frame_count, 0);
  // peek_batch reserved exactly header_size up front and wrote the payload right after it.
  PBL_ASSERTN(header_len == header_size && header_len + payload_len <= sizeof(message));

  if (!bt_driver_audio_companion_notify_data(message, header_len + payload_len)) {
    s_send_backpressure_events++;
    return false;
  }
  audio_companion_spool_mark_sent_through(first_sequence + frame_count - 1);
  s_sent_frames += frame_count;
  return true;
}

//! Stop capturing when a streaming session has heard nothing from the receiver for too long.
//! Runs off the drain timer (active only while streaming). Catches the case where the receiver
//! vanished without the watch seeing a BLE disconnect, so the mic would otherwise run forever.
static void prv_check_receiver_liveness_locked(void) {
  if (s_state != AudioCompanionServiceStateStreaming || s_receiver_presumed_gone ||
      s_silence_suppressing) {
    return;
  }
  if ((prv_uptime_ms() - s_last_receiver_activity_ms) < RECEIVER_LIVENESS_TIMEOUT_MS) {
    return;
  }
  PBL_LOG_WRN("Audio companion: no receiver activity for %ums; presuming receiver gone",
              (unsigned)RECEIVER_LIVENESS_TIMEOUT_MS);
  s_receiver_presumed_gone = true;
  prv_stop_capture_locked();
  prv_begin_gap_pause_locked(AudioCompanionGapReasonTransportReset);
  prv_reevaluate_locked();  // session no longer ready -> AuthorizedIdle, drain timer stops
}

//! Returns true if the spool still has frames to send but the per-callback batch cap was hit, so
//! the caller should re-post a drain to continue (instead of holding the system task in one long
//! callback). A return of false means there is nothing left to do or we stopped on BLE
//! backpressure (the repeating drain timer retries that case).
static bool prv_drain_locked(void) {
  prv_check_receiver_liveness_locked();
  audio_companion_spool_apply_pressure_policy();
  if (!prv_session_ready_locked() || !s_stream_active) {
    return false;
  }
  if (s_need_stream_start) {
    prv_send_stream_start_locked();
    if (s_need_stream_start) {
      return false;  // backpressure; retry on next drain
    }
  }
  if (!prv_send_pending_gap_locked()) {
    return false;
  }
  bool more_pending = false;
  uint32_t batches_sent = 0;
  while (audio_companion_spool_frames_pending_send() > 0) {
    if (!prv_send_data_batch_locked()) {
      break;  // BLE backpressure: the drain timer will retry shortly
    }
    if (++batches_sent >= DRAIN_MAX_BATCHES_PER_CALL) {
      // Yield with work still queued; continue on a fresh callback so the watchdog is fed.
      more_pending = audio_companion_spool_frames_pending_send() > 0;
      break;
    }
  }
  if (s_catch_up_burst && audio_companion_spool_frames_pending_send() == 0) {
    s_catch_up_burst = false;
    if (s_catch_up_timer != TIMER_INVALID_ID) {
      new_timer_stop(s_catch_up_timer);
    }
    prv_update_ble_responsiveness_locked();
  }
  if (s_silence_suppressing && audio_companion_spool_frames_pending_send() == 0 &&
      !audio_companion_spool_has_pending_gap()) {
    prv_stop_drain_timer_locked();
  }
  return more_pending;
}

static void prv_drain_system_task_cb(void *data) {
  mutex_lock(s_lock);
  s_drain_cb_pending = false;
  const bool more_pending = prv_drain_locked();
  mutex_unlock(s_lock);
  prv_apply_capture();  // the liveness check above can drop capture intent
  if (more_pending) {
    prv_post_drain_cb();
  }
}

//! Queue a drain on the system task, coalescing so at most one drain callback is ever outstanding.
//! The audio path posts a drain on (roughly) every push threshold and every drain-timer tick;
//! without coalescing those would pile up on the shared, 30-deep system-task queue and a transient
//! KernelBG stall could overflow it, which the OS treats as a fatal EventQueueFull reboot. Must be
//! called without s_lock held.
static void prv_post_drain_cb(void) {
  mutex_lock(s_lock);
  const bool already_pending = s_drain_cb_pending;
  s_drain_cb_pending = true;
  mutex_unlock(s_lock);
  if (!already_pending) {
    system_task_add_callback(prv_drain_system_task_cb, NULL);
  }
}

static void prv_drain_timer_cb(void *data) {
  prv_post_drain_cb();
}

static void prv_start_drain_timer_locked(void) {
  if (s_drain_timer == TIMER_INVALID_ID) {
    s_drain_timer = new_timer_create();
  }
  new_timer_start(s_drain_timer, DRAIN_PERIOD_MS, prv_drain_timer_cb, NULL,
                  TIMER_START_FLAG_REPEATING);
}

static void prv_stop_drain_timer_locked(void) {
  if (s_drain_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_drain_timer);
  }
}

// ---- State machine ----

static void prv_begin_stream_locked(void) {
  uint32_t id = rand32();
  if (id == 0) {
    id = 1;
  }
  s_stream_id = id;
  s_next_sequence = 0;
  s_next_sample_index = 0;
  s_offline_baseline_dropped = 0;
  s_alert_baseline_dropped = 0;
  s_pending_resume_gap_reason = 0;  // a fresh stream carries no pending gap
  prv_reset_silence_suppression_locked();
  audio_companion_spool_reset();
  s_stream_active = true;
  s_need_stream_start = true;
  s_stream_resumed = false;  // a fresh stream begins at sequence 0
  prv_note_receiver_activity_locked();  // arm the liveness watchdog from stream start
}

//! Re-announce an already-running stream to a freshly (re)attached receiver. The receiver is a new
//! GATT session with no stream context, so it would silently drop resumed STREAM_DATA unless we
//! resend STREAM_START first. The RESUME flag tells it to take the first frame's sequence as the
//! contiguity base; rewinding makes every un-checkpointed frame resend, so buffered audio that
//! piled up while the receiver was away is delivered instead of lost.
static void prv_request_stream_reannounce_locked(void) {
  if (!s_stream_active) {
    return;
  }
  s_need_stream_start = true;
  s_stream_resumed = true;
  audio_companion_spool_rewind_unsent();
  prv_begin_catch_up_burst_locked();
}

static void prv_end_stream_locked(uint8_t stop_reason) {
  if (!s_stream_active) {
    return;
  }
  prv_record_silence_gap_locked();
  if (prv_session_ready_locked() && !s_need_stream_start) {
    prv_send_pending_gap_locked();
  }
  prv_send_stream_stop_locked(stop_reason);
  s_stream_active = false;
  s_need_stream_start = false;
  s_pending_resume_gap_reason = 0;
  prv_reset_silence_suppression_locked();
  audio_companion_spool_reset();
}

//! Single decision point: derives the externally visible state and the
//! capture/stream activity from the current inputs.
static void prv_reevaluate_locked(void) {
  if (!s_initialized) {
    return;
  }
  audio_companion_spool_apply_pressure_policy();

  // The current receiver session is no longer ready (disconnect, unsubscribe, or liveness
  // watchdog): the next ready transition must re-announce the stream to whatever attaches.
  if (!prv_session_ready_locked()) {
    s_stream_attached = false;
  }

  if (!s_enabled) {
    prv_stop_capture_locked();
    prv_end_stream_locked(AudioCompanionStopReasonUserDisabled);
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStateDisabled);
    return;
  }

  if (s_error) {
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStateError);
    return;
  }

  if (s_mic_conflict_active) {
    // Capture intent was already dropped in mic_conflict_begin; this also clears the
    // "mic unavailable" latch so capture is retried once the conflict ends.
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedConflict);
    return;
  }

  if (s_low_battery) {
    if (s_owns_mic) {
      prv_record_silence_gap_locked();
      prv_begin_gap_pause_locked(AudioCompanionGapReasonLowBattery);
    }
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedLowBattery);
    return;
  }

  if (s_receiver_pause_requested) {
    if (s_owns_mic) {
      prv_record_silence_gap_locked();
      prv_begin_gap_pause_locked(AudioCompanionGapReasonUserDisabled);
    }
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedPolicy);
    return;
  }

  if (prv_power_save_active_locked()) {
    if (s_owns_mic) {
      prv_record_silence_gap_locked();
      prv_begin_gap_pause_locked(AudioCompanionGapReasonPowerSave);
    }
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedPowerSave);
    return;
  }

  if (prv_session_ready_locked()) {
    if (!s_stream_active) {
      prv_begin_stream_locked();
    } else if (!s_stream_attached) {
      // A ready session is (re)attaching to an ongoing stream (reconnect or liveness revival).
      // The receiver is a fresh GATT session with no stream context, so re-announce STREAM_START
      // and resend buffered frames before any new data; finish the disconnect gap (if capture had
      // parked) so loss is reported exactly once.
      prv_request_stream_reannounce_locked();
      prv_finish_gap_pause_locked();
    } else {
      prv_finish_gap_pause_locked();
    }
    s_stream_attached = true;
    if (s_capture_start_failed) {
      // A previous apply could not take the mic (dictation will fire the conflict hook). Stay
      // parked rather than re-arming intent, which would spin the applier on a doomed start.
      prv_stop_drain_timer_locked();
      prv_set_state_locked(AudioCompanionServiceStatePausedConflict);
      return;
    }
    prv_start_capture_locked();
    prv_start_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStateStreaming);
    return;
  }

  // Enabled but no ready receiver session.
  prv_stop_drain_timer_locked();
  if (s_owns_mic && s_stream_active) {
    // Brief-disconnect bridge: keep capturing into the spool until overflow
    // (the mic handler parks capture when dropping starts).
    prv_set_state_locked(s_session.authorized
                             ? AudioCompanionServiceStateAuthorizedIdle
                             : AudioCompanionServiceStateIdle);
    return;
  }
  prv_record_silence_gap_locked();
  prv_stop_capture_locked();
  prv_set_state_locked(s_session.authorized ? AudioCompanionServiceStateAuthorizedIdle
                                            : AudioCompanionServiceStateIdle);
}

// ---- Consent flow ----

static void prv_consent_timeout_system_task_cb(void *data) {
  audio_companion_handle_consent_response(false);
}

static void prv_consent_timer_cb(void *data) {
  system_task_add_callback(prv_consent_timeout_system_task_cb, NULL);
}

static void prv_enable_timeout_system_task_cb(void *data) {
  audio_companion_handle_enable_response(false);
}

static void prv_enable_timer_cb(void *data) {
  system_task_add_callback(prv_enable_timeout_system_task_cb, NULL);
}

static void prv_handle_auth_request_locked(const AudioCompanionAuthRequest *req) {
  const uint8_t granted = (req->proto_version < AUDIO_COMPANION_PROTOCOL_VERSION)
                              ? req->proto_version
                              : AUDIO_COMPANION_PROTOCOL_VERSION;
  if (!s_enabled) {
    prv_send_auth_result_locked(req->request_token, AudioCompanionAuthStatusDeniedDisabled, 0);
    return;
  }

  switch (audio_companion_auth_evaluate(req->receiver_id)) {
    case AudioCompanionAuthEvalMatch:
      PBL_LOG_INFO("Audio companion auth: receiver match, session authorized");
      s_session.authorized = true;
      memcpy(s_session.receiver_id, req->receiver_id, sizeof(s_session.receiver_id));
      s_session.granted_proto_version = granted;
      prv_send_auth_result_locked(req->request_token, AudioCompanionAuthStatusOk, granted);
      prv_reevaluate_locked();
      break;
    case AudioCompanionAuthEvalMismatch:
      PBL_LOG_WRN("Audio companion auth denied: receiver mismatch");
      prv_send_auth_result_locked(req->request_token, AudioCompanionAuthStatusDeniedMismatch,
                                  0);
      break;
    case AudioCompanionAuthEvalNoReceiver:
      if (s_consent.pending) {
        prv_send_auth_result_locked(req->request_token,
                                    AudioCompanionAuthStatusPendingUserConsent, 0);
        break;
      }
      if (!s_consent_handler) {
        PBL_LOG_WRN("Audio companion auth denied: no consent handler registered");
        prv_send_auth_result_locked(req->request_token, AudioCompanionAuthStatusInvalid, 0);
        break;
      }
      s_consent = (ConsentRequest){
        .pending = true,
        .request_token = req->request_token,
        .proto_version = req->proto_version,
      };
      memcpy(s_consent.receiver_id, req->receiver_id, sizeof(s_consent.receiver_id));
      strncpy(s_consent.name, req->name, AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES);
      prv_send_auth_result_locked(req->request_token,
                                  AudioCompanionAuthStatusPendingUserConsent, 0);
      if (s_consent_timer == TIMER_INVALID_ID) {
        s_consent_timer = new_timer_create();
      }
      new_timer_start(s_consent_timer, CONSENT_TIMEOUT_MS, prv_consent_timer_cb, NULL, 0);
      PBL_LOG_INFO("Audio companion auth: no receiver bound; requesting user consent");
      // NOTE: invoked on the system task with s_lock held. Handlers must defer
      // any UI work to KernelMain and must not call back into this service
      // synchronously.
      s_consent_handler(s_consent.name);
      break;
  }
}

void audio_companion_handle_consent_response(bool granted) {
  mutex_lock(s_lock);
  if (!s_consent.pending) {
    mutex_unlock(s_lock);
    return;
  }
  s_consent.pending = false;
  if (s_consent_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_consent_timer);
  }
  PBL_LOG_INFO("Audio companion consent response: %s", granted ? "granted" : "declined");

  const uint8_t token = s_consent.request_token;
  if (granted && audio_companion_auth_store_receiver(s_consent.receiver_id, s_consent.name)) {
    const uint8_t granted_version =
        (s_consent.proto_version < AUDIO_COMPANION_PROTOCOL_VERSION)
            ? s_consent.proto_version
            : AUDIO_COMPANION_PROTOCOL_VERSION;
    s_session.authorized = true;
    memcpy(s_session.receiver_id, s_consent.receiver_id, sizeof(s_session.receiver_id));
    s_session.granted_proto_version = granted_version;
    prv_send_auth_result_locked(token, AudioCompanionAuthStatusOk, granted_version);
    prv_reevaluate_locked();
  } else {
    prv_send_auth_result_locked(token, AudioCompanionAuthStatusInvalid, 0);
  }
  memset(&s_consent, 0, sizeof(s_consent));
  mutex_unlock(s_lock);
  prv_apply_capture();
}

void audio_companion_set_consent_handler(AudioCompanionConsentHandler handler) {
  mutex_lock(s_lock);
  s_consent_handler = handler;
  mutex_unlock(s_lock);
}

static void prv_handle_enable_request_locked(const AudioCompanionEnableRequestMsg *req) {
  if (s_enabled) {
    prv_send_ack_locked(req->request_token, AudioCompanionAckStatusOk);
    return;
  }
  if (s_enable_request.pending) {
    prv_send_ack_locked(req->request_token, AudioCompanionAckStatusBadState);
    return;
  }
  if (!s_enable_handler) {
    prv_send_ack_locked(req->request_token, AudioCompanionAckStatusRejected);
    return;
  }

  s_enable_request = (EnableRequest) {
    .pending = true,
    .request_token = req->request_token,
  };
  if (s_enable_timer == TIMER_INVALID_ID) {
    s_enable_timer = new_timer_create();
  }
  new_timer_start(s_enable_timer, CONSENT_TIMEOUT_MS, prv_enable_timer_cb, NULL, 0);
  PBL_LOG_INFO("Audio companion: requesting user approval to enable background audio");
  s_enable_handler();
}

void audio_companion_handle_enable_response(bool granted) {
  mutex_lock(s_lock);
  if (!s_enable_request.pending) {
    mutex_unlock(s_lock);
    return;
  }
  const uint8_t token = s_enable_request.request_token;
  s_enable_request.pending = false;
  if (s_enable_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_enable_timer);
  }
  PBL_LOG_INFO("Audio companion enable response: %s", granted ? "granted" : "declined");
  if (granted) {
    shell_prefs_set_audio_companion_enabled(true);
    if (!s_enabled) {
      s_enabled = true;
      prv_reevaluate_locked();
    }
    prv_send_ack_locked(token, AudioCompanionAckStatusOk);
  } else {
    prv_send_ack_locked(token, AudioCompanionAckStatusRejected);
  }
  memset(&s_enable_request, 0, sizeof(s_enable_request));
  mutex_unlock(s_lock);
  prv_apply_capture();
}

void audio_companion_set_enable_handler(AudioCompanionEnableHandler handler) {
  mutex_lock(s_lock);
  s_enable_handler = handler;
  mutex_unlock(s_lock);
}

// ---- Control message handling ----

static void prv_handle_checkpoint_locked(const AudioCompanionCheckpointMsg *checkpoint) {
  if (!s_session.authorized) {
    prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
    return;
  }
  if (!s_stream_active || checkpoint->stream_id != s_stream_id) {
    prv_send_ack_locked(checkpoint->request_token, AudioCompanionAckStatusRejected);
    return;
  }
  audio_companion_spool_trim_through(checkpoint->highest_contiguous_sequence_persisted);
  audio_companion_spool_apply_pressure_policy();

  const bool pause_requested =
      (checkpoint->receiver_flags & (AUDIO_COMPANION_RECEIVER_FLAG_PAUSE_REQUESTED |
                                     AUDIO_COMPANION_RECEIVER_FLAG_LOW_STORAGE)) != 0;
  prv_send_ack_locked(checkpoint->request_token, AudioCompanionAckStatusOk);
  if (pause_requested != s_receiver_pause_requested) {
    s_receiver_pause_requested = pause_requested;
    prv_reevaluate_locked();
  }
}

static void prv_handle_control_msg_locked(const AudioCompanionControlMsg *msg) {
  switch (msg->msg_id) {
    case AudioCompanionCtrlMsgIdAuthRequest:
      prv_handle_auth_request_locked(&msg->auth_request);
      break;
    case AudioCompanionCtrlMsgIdAuthRevoke:
      if (s_session.authorized &&
          memcmp(msg->auth_revoke.receiver_id, s_session.receiver_id,
                 AUDIO_COMPANION_RECEIVER_ID_BYTES) == 0) {
        audio_companion_auth_forget_receiver();
        prv_end_stream_locked(AudioCompanionStopReasonPolicy);
        s_session.authorized = false;
        prv_send_revoked_locked(AudioCompanionRevokedReasonAppRequested);
        prv_reevaluate_locked();
      } else {
        prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
      }
      break;
    case AudioCompanionCtrlMsgIdCheckpoint:
      prv_handle_checkpoint_locked(&msg->checkpoint);
      break;
    case AudioCompanionCtrlMsgIdPauseRequest:
      if (!s_session.authorized) {
        prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
        break;
      }
      prv_send_ack_locked(msg->pause_request.request_token, AudioCompanionAckStatusOk);
      if (msg->pause_request.reason == AudioCompanionPauseReasonUser) {
        // Explicit user stop: end the stream so the next resume opens a fresh segment instead
        // of replaying the pre-stop spool. Policy/low-storage pauses keep the stream so a
        // transient pause resumes the same recording.
        prv_end_stream_locked(AudioCompanionStopReasonUserDisabled);
      }
      s_receiver_pause_requested = true;
      prv_reevaluate_locked();
      break;
    case AudioCompanionCtrlMsgIdResumeRequest:
      if (!s_session.authorized) {
        prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
        break;
      }
      prv_send_ack_locked(msg->resume_request.request_token, AudioCompanionAckStatusOk);
      if (s_receiver_pause_requested) {
        s_receiver_pause_requested = false;
        prv_reevaluate_locked();
      }
      break;
    case AudioCompanionCtrlMsgIdReceiverHealth:
      if (!s_session.authorized) {
        prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
        break;
      }
      PBL_LOG_DBG("Audio receiver health: battery %u pct, app state %u, queue %" PRIu32,
                  msg->receiver_health.battery_pct, msg->receiver_health.app_state,
                  msg->receiver_health.queue_depth_frames);
      prv_send_ack_locked(msg->receiver_health.request_token, AudioCompanionAckStatusOk);
      break;
    case AudioCompanionCtrlMsgIdEnableRequest:
      prv_handle_enable_request_locked(&msg->enable_request);
      break;
    default:
      break;
  }
}

// ---- bt_driver entry points (BT host task -> system task handoff) ----

typedef struct {
  size_t length;
  uint8_t data[];
} ControlWriteWork;

static void prv_control_write_system_task_cb(void *data) {
  ControlWriteWork *work = data;
  AudioCompanionControlMsg msg;
  const AudioCompanionParseResult result =
      audio_companion_protocol_parse_control(work->data, work->length, &msg);

  mutex_lock(s_lock);
  switch (result) {
    case AudioCompanionParseResultOk: {
      const bool was_presumed_gone = s_receiver_presumed_gone;
      prv_note_receiver_activity_locked();
      prv_handle_control_msg_locked(&msg);
      if (was_presumed_gone) {
        // Receiver came back after the liveness watchdog tripped; resume streaming.
        prv_reevaluate_locked();
      }
      break;
    }
    case AudioCompanionParseResultMalformed:
      PBL_LOG_WRN("Audio companion: malformed control write (%u bytes)",
                  (unsigned)work->length);
      prv_send_error_locked(AudioCompanionErrorCodeMalformed, 0);
      break;
    case AudioCompanionParseResultUnknown:
      break;
  }
  mutex_unlock(s_lock);
  prv_apply_capture();
  kernel_free(work);
}

void audio_companion_handle_control_write(const uint8_t *data, size_t length) {
  if (!s_initialized || !data || length == 0 || length > 128) {
    return;
  }
  ControlWriteWork *work = kernel_malloc(sizeof(ControlWriteWork) + length);
  if (!work) {
    return;
  }
  work->length = length;
  memcpy(work->data, data, length);
  system_task_add_callback(prv_control_write_system_task_cb, work);
}

static void prv_subscription_system_task_cb(void *data) {
  const uintptr_t packed = (uintptr_t)data;
  mutex_lock(s_lock);
  s_session.data_subscribed = (packed & 1) != 0;
  s_session.control_subscribed = (packed & 2) != 0;
  PBL_LOG_INFO("Audio companion subscriptions: data=%u control=%u",
               (unsigned)s_session.data_subscribed, (unsigned)s_session.control_subscribed);
  if (s_session.data_subscribed || s_session.control_subscribed) {
    prv_note_receiver_activity_locked();  // a (re)subscribe is proof the receiver is present
  }
  if (prv_session_ready_locked()) {
    s_offline_baseline_dropped = 0;
  }
  // Re-announce on reconnect is centralized in prv_reevaluate_locked (keyed off s_stream_attached)
  // so it fires regardless of whether the receiver subscribes before or after AUTH — iOS subscribes
  // both characteristics first, so doing it here would miss the common reconnect.
  prv_reevaluate_locked();
  mutex_unlock(s_lock);
  prv_apply_capture();
}

void audio_companion_handle_subscription_change(bool data_subscribed,
                                                bool control_subscribed) {
  if (!s_initialized) {
    return;
  }
  const uintptr_t packed = (data_subscribed ? 1 : 0) | (control_subscribed ? 2 : 0);
  system_task_add_callback(prv_subscription_system_task_cb, (void *)packed);
}

static void prv_disconnect_system_task_cb(void *data) {
  mutex_lock(s_lock);
  PBL_LOG_INFO("Audio companion: receiver disconnected; session reset");
  s_session.authorized = false;
  s_session.data_subscribed = false;
  s_session.control_subscribed = false;
  memset(s_session.receiver_id, 0, sizeof(s_session.receiver_id));
  // The policy pause is the receiver's session-scoped request; clear it so the watch is never
  // stranded in PausedPolicy once the receiver is gone. The phone re-asserts pause on reconnect
  // if it still wants one (declarative reconcile on the app side).
  s_receiver_pause_requested = false;
  s_receiver_presumed_gone = false;
  if (s_consent.pending) {
    s_consent.pending = false;
    if (s_consent_timer != TIMER_INVALID_ID) {
      new_timer_stop(s_consent_timer);
    }
    memset(&s_consent, 0, sizeof(s_consent));
  }
  if (s_stream_active) {
    AudioCompanionSpoolStats stats;
    audio_companion_spool_get_stats(&stats);
    s_offline_baseline_dropped = stats.dropped_overflow_frames;
    audio_companion_spool_rewind_unsent();
  }
  prv_reevaluate_locked();
  mutex_unlock(s_lock);
  prv_apply_capture();
}

void audio_companion_handle_disconnect(void) {
  if (!s_initialized) {
    return;
  }
  system_task_add_callback(prv_disconnect_system_task_cb, NULL);
}

void audio_companion_fill_info(uint8_t *buf, size_t *length_in_out) {
  if (!buf || !length_in_out) {
    return;
  }
  mutex_lock(s_lock);
  uint8_t flags = 0;
  if (audio_companion_auth_receiver_exists()) {
    flags |= AUDIO_COMPANION_INFO_FLAG_RECEIVER_BOUND;
  }
  if (s_enabled) {
    flags |= AUDIO_COMPANION_INFO_FLAG_ENABLED;
  }
  if (s_consent.pending) {
    flags |= AUDIO_COMPANION_INFO_FLAG_CONSENT_PENDING;
  }
  const AudioCompanionInfo info = {
    .info_version = 1,
    .protocol_min = AUDIO_COMPANION_PROTOCOL_VERSION,
    .protocol_max = AUDIO_COMPANION_PROTOCOL_VERSION,
    .service_state = (uint8_t)s_state,
    .codec_bitmap = 0x01,  // Speex wideband
    .flags = flags,
  };
  *length_in_out = audio_companion_protocol_build_info(buf, *length_in_out, &info);
  mutex_unlock(s_lock);
  PBL_LOG_INFO("Audio companion info read (state=%u flags=0x%02x)", info.service_state,
               info.flags);
}

// ---- Mic arbitration (called from the voice service) ----

void audio_companion_mic_conflict_begin(void) {
  if (!s_initialized) {
    return;
  }
  mutex_lock(s_lock);
  if (s_owns_mic) {
    prv_record_silence_gap_locked();
    prv_begin_gap_pause_locked(AudioCompanionGapReasonMicConflict);
    s_mic_conflicts++;
  }
  prv_stop_capture_locked();
  s_mic_conflict_active = true;
  prv_reevaluate_locked();
  mutex_unlock(s_lock);
  // Synchronous: the voice service calls this immediately before claiming the mic itself, so the
  // driver must actually be released before we return.
  prv_apply_capture();
}

void audio_companion_mic_conflict_end(void) {
  if (!s_initialized) {
    return;
  }
  mutex_lock(s_lock);
  s_mic_conflict_active = false;
  prv_reevaluate_locked();
  mutex_unlock(s_lock);
  prv_apply_capture();
}

// ---- Battery policy ----

static void prv_battery_event_handler(PebbleEvent *event, void *context) {
  const BatteryChargeState charge = battery_get_charge_state();
  mutex_lock(s_lock);
  bool low = s_low_battery;
  if (charge.charge_percent < CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT) {
    low = true;
  } else if (charge.charge_percent >=
             CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT + LOW_BATTERY_RESUME_HYSTERESIS_PCT) {
    low = false;
  }
  if (low != s_low_battery) {
    s_low_battery = low;
    prv_reevaluate_locked();
  }
  mutex_unlock(s_lock);
  prv_apply_capture();
}

// ---- Public API ----

//! Record why the previous session ended into the persisted reboot ring, so a user who notices
//! the watch restarted can read the fault class from Settings. The OS captured the reason in a
//! battery-backed register at boot; we just persist it alongside whether background audio was on.
//! Does its own flash I/O, so it must run without the service lock held.
static void prv_record_boot_reboot_trace(void) {
  const uint8_t reason = (uint8_t)reboot_reason_get_last_reboot_reason();
  const bool enabled = shell_prefs_get_audio_companion_enabled();

  AudioCompanionRebootTrace trace;
  audio_companion_reboot_trace_load(&trace);
  audio_companion_reboot_trace_record(&trace, reason, enabled, (uint32_t)rtc_get_time());
  audio_companion_reboot_trace_save(&trace);

  mutex_lock(s_lock);
  s_reboot_trace = trace;
  mutex_unlock(s_lock);

  if (audio_companion_reboot_trace_is_error_reason(reason)) {
    PBL_LOG_WRN("Audio companion: previous restart was a fault (%s); %u faults / %u boots logged",
                audio_companion_reboot_trace_reason_name(reason),
                (unsigned)trace.total_error_reboots, (unsigned)trace.total_reboots);
  } else {
    PBL_LOG_INFO("Audio companion: previous restart reason: %s",
                 audio_companion_reboot_trace_reason_name(reason));
  }
}

void audio_companion_init(void) {
  s_lock = mutex_create();
  s_capture_lock = mutex_create();
  audio_companion_spool_init();
  audio_companion_auth_init();

  mutex_lock(s_lock);
  audio_companion_reboot_trace_clear(&s_reboot_trace);
  s_initialized = true;
  s_enabled = shell_prefs_get_audio_companion_enabled();
  s_pause_stationary_enabled =
      shell_prefs_get_audio_companion_pause_stationary_enabled();
  s_pause_low_power_enabled =
      shell_prefs_get_audio_companion_pause_low_power_enabled();
  s_silence_mode =
      (AudioCompanionSilenceMode)shell_prefs_get_audio_companion_silence_mode();
  const BatteryChargeState charge = battery_get_charge_state();
  s_low_battery = charge.charge_percent < CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT;
  prv_reevaluate_locked();
  mutex_unlock(s_lock);
  prv_apply_capture();

  s_battery_event_info = (EventServiceInfo){
    .type = PEBBLE_BATTERY_STATE_CHANGE_EVENT,
    .handler = prv_battery_event_handler,
  };
  event_service_client_subscribe(&s_battery_event_info);
}

void audio_companion_handle_prefs_loaded(void) {
  // The settings live in the shell prefs file, which shell_prefs_init() loads only after this
  // service has already initialized at boot. So audio_companion_init() above necessarily reads
  // compile-time defaults. Re-read the settings here, once the prefs are actually loaded, so a
  // reboot (which connection resets can trigger) restores the user's choices instead of defaults.
  if (!s_initialized) {
    return;
  }
  mutex_lock(s_lock);
  s_enabled = shell_prefs_get_audio_companion_enabled();
  s_pause_stationary_enabled = shell_prefs_get_audio_companion_pause_stationary_enabled();
  s_pause_low_power_enabled = shell_prefs_get_audio_companion_pause_low_power_enabled();
  s_silence_mode =
      (AudioCompanionSilenceMode)shell_prefs_get_audio_companion_silence_mode();
  prv_reevaluate_locked();
  const bool record_reboot = !s_reboot_trace_recorded;
  s_reboot_trace_recorded = true;
  mutex_unlock(s_lock);
  prv_apply_capture();

  // Now that the persisted prefs are loaded (so the "enabled" flag is accurate) capture the
  // previous reboot reason into the flight recorder. Done once per boot, outside the lock.
  if (record_reboot) {
    prv_record_boot_reboot_trace();
  }
}

void audio_companion_get_reboot_trace(struct AudioCompanionRebootTrace *out) {
  if (!out) {
    return;
  }
  mutex_lock(s_lock);
  *out = s_reboot_trace;
  mutex_unlock(s_lock);
}

bool audio_companion_is_enabled(void) {
  mutex_lock(s_lock);
  const bool enabled = s_enabled;
  mutex_unlock(s_lock);
  return enabled;
}

void audio_companion_apply_enabled(bool enabled) {
  mutex_lock(s_lock);
  if (s_enabled != enabled) {
    s_enabled = enabled;
    prv_reevaluate_locked();
  }
  mutex_unlock(s_lock);
  prv_apply_capture();
}

void audio_companion_set_enabled(bool enabled) {
  shell_prefs_set_audio_companion_enabled(enabled);
  audio_companion_apply_enabled(enabled);
}

void audio_companion_set_runlevel(RunLevel runlevel) {
  if (!s_initialized) {
    return;
  }
  mutex_lock(s_lock);
  if (s_runlevel != runlevel) {
    s_runlevel = runlevel;
    prv_reevaluate_locked();
  }
  mutex_unlock(s_lock);
  prv_apply_capture();
}

bool audio_companion_get_pause_stationary_enabled(void) {
  mutex_lock(s_lock);
  const bool enabled = s_pause_stationary_enabled;
  mutex_unlock(s_lock);
  return enabled;
}

void audio_companion_set_pause_stationary_enabled(bool enabled) {
  shell_prefs_set_audio_companion_pause_stationary_enabled(enabled);
  mutex_lock(s_lock);
  if (s_pause_stationary_enabled != enabled) {
    s_pause_stationary_enabled = enabled;
    prv_reevaluate_locked();
  }
  mutex_unlock(s_lock);
  prv_apply_capture();
}

bool audio_companion_get_pause_low_power_enabled(void) {
  mutex_lock(s_lock);
  const bool enabled = s_pause_low_power_enabled;
  mutex_unlock(s_lock);
  return enabled;
}

void audio_companion_set_pause_low_power_enabled(bool enabled) {
  shell_prefs_set_audio_companion_pause_low_power_enabled(enabled);
  mutex_lock(s_lock);
  if (s_pause_low_power_enabled != enabled) {
    s_pause_low_power_enabled = enabled;
    prv_reevaluate_locked();
  }
  mutex_unlock(s_lock);
  prv_apply_capture();
}

bool audio_companion_get_silence_suppression_enabled(void) {
  mutex_lock(s_lock);
  const bool enabled = s_silence_mode != AudioCompanionSilenceModeOff;
  mutex_unlock(s_lock);
  return enabled;
}

void audio_companion_set_silence_suppression_enabled(bool enabled) {
  shell_prefs_set_audio_companion_silence_suppression_enabled(enabled);
  audio_companion_set_silence_mode(enabled ? AudioCompanionSilenceModeLight
                                           : AudioCompanionSilenceModeOff);
}

AudioCompanionSilenceMode audio_companion_get_silence_mode(void) {
  mutex_lock(s_lock);
  const AudioCompanionSilenceMode mode = s_silence_mode;
  mutex_unlock(s_lock);
  return mode;
}

void audio_companion_set_silence_mode(AudioCompanionSilenceMode mode) {
  if (mode >= AudioCompanionSilenceModeCount) {
    return;
  }
  shell_prefs_set_audio_companion_silence_mode((uint8_t)mode);
  shell_prefs_set_audio_companion_silence_suppression_enabled(
      mode != AudioCompanionSilenceModeOff);
  mutex_lock(s_lock);
  if (s_silence_mode != mode) {
    if (mode == AudioCompanionSilenceModeOff) {
      prv_record_silence_gap_locked();
      if (prv_session_ready_locked()) {
        prv_start_drain_timer_locked();
      }
    }
    s_silence_mode = mode;
  }
  mutex_unlock(s_lock);
}

AudioCompanionServiceState audio_companion_get_state(void) {
  mutex_lock(s_lock);
  const AudioCompanionServiceState state = s_state;
  mutex_unlock(s_lock);
  return state;
}

void audio_companion_get_diagnostics(AudioCompanionDiagnostics *diag_out) {
  if (!diag_out) {
    return;
  }
  mutex_lock(s_lock);
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  *diag_out = (AudioCompanionDiagnostics){
    .state = s_state,
    .captured_frames = s_captured_frames,
    .sent_frames = s_sent_frames,
    .send_backpressure_events = s_send_backpressure_events,
    .gap_records = stats.gap_records,
    .dropped_overflow_frames = stats.dropped_overflow_frames,
    .mic_conflicts = s_mic_conflicts,
    .suppressed_silence_frames = s_suppressed_silence_frames,
    .spool_bytes = stats.current_bytes,
    .spool_high_water_bytes = stats.high_water_bytes,
    .loss_alerts_posted = s_loss_alerts_posted,
  };
  mutex_unlock(s_lock);
}

bool audio_companion_get_receiver_name(char *buf, size_t buf_size) {
  mutex_lock(s_lock);
  const bool result = audio_companion_auth_get_receiver_name(buf, buf_size);
  mutex_unlock(s_lock);
  return result;
}

void audio_companion_forget_receiver(void) {
  mutex_lock(s_lock);
  audio_companion_auth_forget_receiver();
  prv_end_stream_locked(AudioCompanionStopReasonPolicy);
  if (s_session.authorized) {
    s_session.authorized = false;
    prv_send_revoked_locked(AudioCompanionRevokedReasonUserOnWatch);
  }
  memset(s_session.receiver_id, 0, sizeof(s_session.receiver_id));
  prv_reevaluate_locked();
  mutex_unlock(s_lock);
  prv_apply_capture();
}

#ifdef UNITTEST
//! Lets the mic fakes assert that the driver is never entered while the service lock is held.
//! That ordering is the deadlock: the driver calls prv_mic_data_handler() under its own mutex,
//! and the handler takes s_lock.
PebbleMutex *audio_companion_test_get_lock(void) { return s_lock; }

void audio_companion_test_reset(void) {
  audio_companion_spool_reset();
  s_lock = NULL;
  s_capture_lock = NULL;
  s_initialized = false;
  s_state = AudioCompanionServiceStateDisabled;
  s_enabled = false;
  memset(&s_session, 0, sizeof(s_session));
  memset(&s_consent, 0, sizeof(s_consent));
  memset(&s_enable_request, 0, sizeof(s_enable_request));
  s_consent_handler = NULL;
  s_enable_handler = NULL;
  s_consent_timer = TIMER_INVALID_ID;
  s_enable_timer = TIMER_INVALID_ID;
  s_owns_mic = false;
  s_capture_wanted = false;
  s_capture_start_failed = false;
  s_mic_started = false;
  s_stream_active = false;
  s_need_stream_start = false;
  s_stream_resumed = false;
  s_stream_attached = false;
  s_stream_id = 0;
  s_next_sequence = 0;
  s_next_sample_index = 0;
  s_drain_timer = TIMER_INVALID_ID;
  s_catch_up_timer = TIMER_INVALID_ID;
  s_catch_up_burst = false;
  s_drain_cb_pending = false;
  s_capture_parked = false;
  s_pause_started_ms = 0;
  s_pending_resume_gap_reason = 0;
  s_last_receiver_activity_ms = 0;
  s_receiver_presumed_gone = false;
  s_mic_conflict_active = false;
  s_receiver_pause_requested = false;
  s_low_battery = false;
  s_pause_stationary_enabled = false;
  s_pause_low_power_enabled = false;
  s_silence_mode = AudioCompanionSilenceModeOff;
  s_runlevel = RunLevel_Normal;
  s_error = false;
  s_captured_frames = 0;
  s_sent_frames = 0;
  s_send_backpressure_events = 0;
  s_mic_conflicts = 0;
  s_suppressed_silence_frames = 0;
  s_loss_alerts_posted = 0;
  s_alert_baseline_dropped = 0;
  s_last_alert_uptime_s = 0;
  s_offline_baseline_dropped = 0;
  s_reboot_trace_recorded = false;
  audio_companion_reboot_trace_clear(&s_reboot_trace);
  prv_reset_silence_suppression_locked();
  memset(&s_battery_event_info, 0, sizeof(s_battery_event_info));
}
#endif
