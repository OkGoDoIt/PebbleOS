/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/background_audio.h"
#include "pbl/services/background_audio_private.h"
#include "spool.h"

#include "board/board.h"
#include "bluetooth/responsiveness.h"
#include "comm/bt_lock.h"
#include "drivers/mic.h"
#include "os/mutex.h"
#include "pbl/services/bluetooth/bluetooth_persistent_storage.h"
#include "pbl/services/comm_session/session.h"
#include "pbl/services/comm_session/session_send_buffer.h"
#include "pbl/services/new_timer/new_timer.h"
#include "applib/event_service_client.h"
#include "kernel/events.h"
#include "pbl/services/voice/voice_speex.h"
#include "shell/prefs.h"
#include "system/logging.h"
#include "system/passert.h"
#include "pbl/services/system_task.h"

#include <inttypes.h>
#include <string.h>

#define AUDIO_ENDPOINT (10000)
#define BACKGROUND_AUDIO_SEND_TIMEOUT_MS (50)
#define BACKGROUND_AUDIO_DRAIN_RETRY_MS (100)
#define BACKGROUND_AUDIO_MAX_FRAMES_PER_PACKET (5)

static PebbleMutex *s_lock;
static BackgroundAudioState s_state = BackgroundAudioStateDisabled;
static bool s_enabled_pref;
static uint32_t s_stream_id;
static uint32_t s_next_sequence;
static uint64_t s_next_sample_index;
static uint32_t s_last_checkpoint_sequence;
static TimerID s_drain_timer = TIMER_INVALID_ID;
static EventServiceInfo s_connection_event_info;
static EventServiceInfo s_capabilities_event_info;

static uint32_t s_captured_frames;
static uint32_t s_sent_frames;
static uint32_t s_send_failures;
static uint32_t s_gap_count;

static void prv_schedule_drain(void);
static void prv_set_state_locked(BackgroundAudioState state);
static void background_audio_mic_data_handler(int16_t *samples, size_t sample_count, void *context);

static bool prv_phone_supports_background_audio(void) {
  PebbleProtocolCapabilities capabilities;
  bt_persistent_storage_get_cached_system_capabilities(&capabilities);
  return capabilities.background_audio_streaming_support;
}

static bool prv_comm_session_connected(void) {
  return comm_session_get_system_session() != NULL;
}

static void prv_send_packet(const uint8_t *data, size_t length) {
  CommSession *comm_session = comm_session_get_system_session();
  if (!comm_session) {
    return;
  }
  comm_session_send_data(comm_session, AUDIO_ENDPOINT, data, length,
                         COMM_SESSION_DEFAULT_TIMEOUT);
}

static void prv_send_stream_start_locked(void) {
  BackgroundAudioStreamStartMsg msg = {
    .command_id = BackgroundAudioMsgIdStreamStart,
    .protocol_version = BACKGROUND_AUDIO_PROTOCOL_VERSION,
    .stream_id = s_stream_id,
    .codec_id = BackgroundAudioCodecSpeexWideband,
    .channels = 1,
    .frame_samples = BACKGROUND_AUDIO_DEFAULT_FRAME_SAMPLES,
    .sample_rate_hz = BACKGROUND_AUDIO_DEFAULT_SAMPLE_RATE_HZ,
    .bit_rate_bps = BACKGROUND_AUDIO_DEFAULT_BIT_RATE_BPS,
    .frame_duration_ms = BACKGROUND_AUDIO_DEFAULT_FRAME_DURATION_MS,
    .start_time_ms = 0,
    .start_monotonic_ms = 0,
    .flags = 0,
  };
  prv_send_packet((const uint8_t *)&msg, sizeof(msg));
}

static void prv_send_stream_stop_locked(uint8_t reason) {
  BackgroundAudioStreamStopMsg msg = {
    .command_id = BackgroundAudioMsgIdStreamStop,
    .stream_id = s_stream_id,
    .reason = reason,
    .final_sequence = (s_next_sequence > 0) ? (s_next_sequence - 1) : 0,
    .final_sample_index = s_next_sample_index,
    .counters_crc_or_zero = 0,
  };
  prv_send_packet((const uint8_t *)&msg, sizeof(msg));
}

static bool prv_send_pending_gap_locked(void) {
  BackgroundAudioSpoolPendingGap gap;
  if (!background_audio_spool_take_pending_gap(&gap)) {
    return false;
  }
  BackgroundAudioStreamGapMsg msg = {
    .command_id = BackgroundAudioMsgIdStreamGap,
    .stream_id = s_stream_id,
    .first_missing_sequence = gap.first_missing_sequence,
    .missing_frame_count = gap.missing_frame_count,
    .first_missing_sample_index = gap.first_missing_sample_index,
    .reason = gap.reason,
    .watch_drop_counter = background_audio_spool_dropped_overflow_count(),
  };
  prv_send_packet((const uint8_t *)&msg, sizeof(msg));
  s_gap_count++;
  return true;
}

static void prv_stop_mic_and_speex_locked(void) {
  if (mic_is_running(MIC)) {
    mic_stop(MIC);
  }
  if (voice_speex_is_initialized()) {
    voice_speex_deinit();
  }
}

static bool prv_start_mic_locked(void) {
  if (!voice_speex_init()) {
    return false;
  }

  int16_t *frame_buffer = voice_speex_get_frame_buffer();
  size_t frame_size_samples = voice_speex_get_frame_size();
  if (!frame_buffer || frame_size_samples == 0) {
    voice_speex_deinit();
    return false;
  }

  if (!mic_start(MIC, background_audio_mic_data_handler, NULL, frame_buffer, frame_size_samples)) {
    if (!mic_is_running(MIC)) {
      voice_speex_deinit();
      return false;
    }
  }
  return true;
}

static void prv_begin_stream_locked(void) {
  s_stream_id++;
  if (s_stream_id == 0) {
    s_stream_id = 1;
  }
  s_next_sequence = 0;
  s_next_sample_index = 0;
  s_last_checkpoint_sequence = 0;
  background_audio_spool_reset();
  prv_send_stream_start_locked();
}

static void prv_enter_recording_locked(void) {
  if (!prv_start_mic_locked()) {
    prv_set_state_locked(BackgroundAudioStateError);
    return;
  }
  prv_begin_stream_locked();
  if (prv_comm_session_connected() && prv_phone_supports_background_audio()) {
    prv_set_state_locked(BackgroundAudioStateRecordingStreaming);
  } else {
    prv_set_state_locked(BackgroundAudioStateRecordingBuffering);
  }
  prv_schedule_drain();
}

static void prv_stop_recording_locked(uint8_t stop_reason) {
  if (s_state == BackgroundAudioStateDisabled || s_state == BackgroundAudioStateEnabledIdle ||
      s_state == BackgroundAudioStatePausedForConflict ||
      s_state == BackgroundAudioStatePausedForPolicy) {
    return;
  }
  BackgroundAudioState previous = s_state;
  prv_set_state_locked(BackgroundAudioStateStopping);
  prv_stop_mic_and_speex_locked();
  if (previous != BackgroundAudioStateStarting) {
    prv_send_stream_stop_locked(stop_reason);
  }
  if (s_enabled_pref && prv_phone_supports_background_audio()) {
    prv_set_state_locked(BackgroundAudioStateEnabledIdle);
  } else if (s_enabled_pref) {
    prv_set_state_locked(BackgroundAudioStateEnabledIdle);
  } else {
    prv_set_state_locked(BackgroundAudioStateDisabled);
  }
}

static void prv_set_state_locked(BackgroundAudioState state) {
  s_state = state;
}

static void prv_update_enabled_state_locked(void) {
  if (!s_enabled_pref) {
    prv_stop_recording_locked(BackgroundAudioStopReasonUserDisabled);
    prv_set_state_locked(BackgroundAudioStateDisabled);
    return;
  }
  if (!background_audio_is_supported()) {
    prv_stop_recording_locked(BackgroundAudioStopReasonPolicy);
    prv_set_state_locked(BackgroundAudioStateEnabledIdle);
    return;
  }
  if (s_state == BackgroundAudioStateDisabled) {
    prv_set_state_locked(BackgroundAudioStateEnabledIdle);
  }
  if (s_state == BackgroundAudioStateEnabledIdle ||
      s_state == BackgroundAudioStatePausedForConflict ||
      s_state == BackgroundAudioStatePausedForPolicy) {
    prv_enter_recording_locked();
  }
}

void background_audio_mic_data_handler(int16_t *samples, size_t sample_count, void *context) {
  (void)context;
  mutex_lock(s_lock);
  if (s_state != BackgroundAudioStateRecordingStreaming &&
      s_state != BackgroundAudioStateRecordingBuffering) {
    mutex_unlock(s_lock);
    return;
  }

  size_t expected_samples = voice_speex_get_frame_size();
  if (sample_count != expected_samples) {
    mutex_unlock(s_lock);
    return;
  }

  uint8_t encoded_buffer[BACKGROUND_AUDIO_MAX_ENCODED_FRAME_BYTES];
  int encoded_bytes = voice_speex_encode_frame(samples, encoded_buffer, sizeof(encoded_buffer));
  if (encoded_bytes <= 0) {
    mutex_unlock(s_lock);
    return;
  }

  const uint32_t sequence = s_next_sequence++;
  const uint64_t sample_index = s_next_sample_index;
  s_next_sample_index += expected_samples;
  s_captured_frames++;

  background_audio_spool_push(sequence, sample_index, encoded_buffer,
                            (uint16_t)encoded_bytes, 0);
  mutex_unlock(s_lock);
  prv_schedule_drain();
}

static bool prv_send_data_batch_locked(void) {
  CommSession *comm_session = comm_session_get_system_session();
  if (!comm_session) {
    return false;
  }

  const size_t max_payload = comm_session_send_buffer_get_max_payload_length(comm_session);
  uint32_t first_sequence = 0;
  uint64_t first_sample_index = 0;
  uint8_t frame_count = 0;
  uint8_t frames_payload[900];
  size_t frames_payload_len = 0;

  if (!background_audio_spool_peek_batch(max_payload, &first_sequence, &first_sample_index,
                                         &frame_count, frames_payload, sizeof(frames_payload),
                                         &frames_payload_len)) {
    return false;
  }

  const size_t packet_size = background_audio_stream_data_header_size() + frames_payload_len;
  SendBuffer *sb = comm_session_send_buffer_begin_write(comm_session, AUDIO_ENDPOINT,
                                                        packet_size,
                                                        BACKGROUND_AUDIO_SEND_TIMEOUT_MS);
  if (!sb) {
    s_send_failures++;
    return false;
  }

  BackgroundAudioStreamDataHeader header = {
    .command_id = BackgroundAudioMsgIdStreamData,
    .stream_id = s_stream_id,
    .first_sequence = first_sequence,
    .first_sample_index = first_sample_index,
    .frame_count = frame_count,
    .flags = 0,
  };
  comm_session_send_buffer_write(sb, (const uint8_t *)&header, sizeof(header));
  comm_session_send_buffer_write(sb, frames_payload, frames_payload_len);
  comm_session_send_buffer_end_write(sb);

  uint32_t last_sequence = first_sequence + frame_count - 1;
  background_audio_spool_pop_through(last_sequence);
  s_sent_frames += frame_count;
  return true;
}

static void prv_drain_task_cb(void *data) {
  (void)data;
  mutex_lock(s_lock);

  if (s_state == BackgroundAudioStateRecordingBuffering &&
      prv_comm_session_connected() && prv_phone_supports_background_audio()) {
    prv_set_state_locked(BackgroundAudioStateRecordingStreaming);
    prv_send_stream_start_locked();
  }

  if (s_state != BackgroundAudioStateRecordingStreaming &&
      s_state != BackgroundAudioStateRecordingBuffering) {
    mutex_unlock(s_lock);
    return;
  }

  if (prv_comm_session_connected() && prv_phone_supports_background_audio()) {
  retry_send:
    if (prv_send_pending_gap_locked()) {
      goto retry_send;
    }
    while (background_audio_spool_depth() > 0) {
      if (!prv_send_data_batch_locked()) {
        break;
      }
    }
  }

  const bool should_retry = background_audio_spool_depth() > 0 ||
      background_audio_spool_has_pending_gap();
  mutex_unlock(s_lock);

  if (should_retry && s_drain_timer != TIMER_INVALID_ID) {
    new_timer_start(s_drain_timer, BACKGROUND_AUDIO_DRAIN_RETRY_MS, prv_drain_task_cb, NULL, 0);
  }
}

static void prv_schedule_drain(void) {
  if (s_drain_timer == TIMER_INVALID_ID) {
    s_drain_timer = new_timer_create();
  }
  system_task_add_callback(prv_drain_task_cb, NULL);
}

static void prv_connection_handler(PebbleEvent *event, void *context) {
  (void)context;
  if (event->bluetooth.comm_session_event.is_open) {
    background_audio_handle_comm_session_changed();
  } else {
    background_audio_handle_comm_session_changed();
  }
}

static void prv_capabilities_handler(PebbleEvent *event, void *context) {
  (void)context;
  if (event->capabilities.flags_diff.background_audio_streaming_support) {
    background_audio_handle_comm_session_changed();
  }
}

void background_audio_init(void) {
  s_lock = mutex_create();
  background_audio_spool_init();
  s_drain_timer = new_timer_create();
  mutex_lock(s_lock);
  s_enabled_pref = shell_prefs_get_background_audio_enabled();
  s_state = s_enabled_pref ? BackgroundAudioStateEnabledIdle : BackgroundAudioStateDisabled;
  if (s_enabled_pref && prv_comm_session_connected() && prv_phone_supports_background_audio()) {
    prv_enter_recording_locked();
  }
  mutex_unlock(s_lock);

  s_connection_event_info = (EventServiceInfo) {
    .type = PEBBLE_BT_CONNECTION_DEBOUNCED_EVENT,
    .handler = prv_connection_handler,
  };
  event_service_client_subscribe(&s_connection_event_info);

  s_capabilities_event_info = (EventServiceInfo) {
    .type = PEBBLE_CAPABILITIES_CHANGED_EVENT,
    .handler = prv_capabilities_handler,
  };
  event_service_client_subscribe(&s_capabilities_event_info);
}

bool background_audio_is_supported(void) {
  return true;
}

bool background_audio_is_enabled(void) {
  mutex_lock(s_lock);
  const bool enabled = s_enabled_pref;
  mutex_unlock(s_lock);
  return enabled;
}

void background_audio_apply_enabled(bool enabled) {
  mutex_lock(s_lock);
  s_enabled_pref = enabled;
  prv_update_enabled_state_locked();
  mutex_unlock(s_lock);
}

void background_audio_set_enabled(bool enabled) {
  shell_prefs_set_background_audio_enabled(enabled);
}

BackgroundAudioState background_audio_get_state(void) {
  mutex_lock(s_lock);
  const BackgroundAudioState state = s_state;
  mutex_unlock(s_lock);
  return state;
}

void background_audio_pause_for_conflict(void) {
  mutex_lock(s_lock);
  if (s_state != BackgroundAudioStateRecordingStreaming &&
      s_state != BackgroundAudioStateRecordingBuffering &&
      s_state != BackgroundAudioStateStarting) {
    mutex_unlock(s_lock);
    return;
  }

  background_audio_spool_record_gap(s_next_sequence, 0, s_next_sample_index,
                                    BackgroundAudioGapReasonMicConflict);
  prv_send_stream_stop_locked(BackgroundAudioStopReasonPolicy);
  prv_stop_mic_and_speex_locked();
  prv_set_state_locked(BackgroundAudioStatePausedForConflict);
  mutex_unlock(s_lock);
  prv_schedule_drain();
}

void background_audio_resume_after_conflict(void) {
  mutex_lock(s_lock);
  if (!s_enabled_pref || s_state != BackgroundAudioStatePausedForConflict) {
    mutex_unlock(s_lock);
    return;
  }
  prv_enter_recording_locked();
  mutex_unlock(s_lock);
}

void background_audio_handle_comm_session_changed(void) {
  mutex_lock(s_lock);
  if (!s_enabled_pref) {
    mutex_unlock(s_lock);
    return;
  }
  if (prv_comm_session_connected() && prv_phone_supports_background_audio()) {
    if (s_state == BackgroundAudioStateEnabledIdle ||
        s_state == BackgroundAudioStatePausedForPolicy) {
      prv_enter_recording_locked();
    } else if (s_state == BackgroundAudioStateRecordingBuffering) {
      prv_send_stream_start_locked();
      prv_set_state_locked(BackgroundAudioStateRecordingStreaming);
      prv_schedule_drain();
    }
  } else if (s_state == BackgroundAudioStateRecordingStreaming) {
    prv_set_state_locked(BackgroundAudioStateRecordingBuffering);
  }
  mutex_unlock(s_lock);
}

void background_audio_handle_inbound_msg(const uint8_t *data, size_t size) {
  BackgroundAudioStreamCheckpointMsg checkpoint;
  if (!background_audio_parse_checkpoint_msg(data, size, &checkpoint)) {
    return;
  }
  mutex_lock(s_lock);
  if (checkpoint.stream_id == s_stream_id) {
    s_last_checkpoint_sequence = checkpoint.highest_contiguous_sequence_persisted;
  }
  mutex_unlock(s_lock);
}
