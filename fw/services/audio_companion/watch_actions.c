/* SPDX-License-Identifier: Apache-2.0 */

#include "watch_actions.h"

#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "pbl/kernel/mutex.h"
#include "pbl/logging/logging.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/system_task.h"

#include <string.h>

PBL_LOG_MODULE_DECLARE(service_audio_companion, CONFIG_SERVICE_AUDIO_COMPANION_LOG_LEVEL);

typedef struct {
  TimerID timer;
  bool active;
  char label[AUDIO_COMPANION_WATCH_ACTION_LABEL_MAX_BYTES + 1];
} WatchTimer;

typedef struct {
  uint8_t action_id;
  WatchActionRequest request;
} PendingAction;

static PBL_MUTEX_DEFINE(s_lock);
static uint8_t s_assembly_id;
static uint16_t s_assembly_length;
static uint8_t s_assembly[AUDIO_COMPANION_WATCH_ACTION_PAYLOAD_MAX_BYTES];
static uint8_t s_choice_action_id;
static WatchTimer s_timers[WATCH_ACTIONS_MAX_TIMERS];
//! Turns Quiet Time back off when the assistant set it for a while.
static TimerID s_quiet_time_timer = TIMER_INVALID_ID;
static WatchActionsChooseHandler s_choose_handler;
static WatchActionsTimerDoneHandler s_timer_done_handler;
static WatchActionsChooseDismissHandler s_dismiss_handler;
static WatchActionsNoticeHandler s_notice_handler;

// ---- parsing ----

typedef struct {
  const uint8_t *data;
  size_t length;
  size_t offset;
  bool ok;
} Reader;

static uint8_t prv_u8(Reader *r) {
  if (r->offset + 1 > r->length) {
    r->ok = false;
    return 0;
  }
  return r->data[r->offset++];
}

static uint16_t prv_u16(Reader *r) {
  const uint16_t lo = prv_u8(r);
  return lo | ((uint16_t)prv_u8(r) << 8);
}

static uint32_t prv_u32(Reader *r) {
  const uint32_t lo = prv_u16(r);
  return lo | ((uint32_t)prv_u16(r) << 16);
}

//! A length-prefixed UTF-8 string into out (size max+1), cut on a character boundary at max.
static void prv_string(Reader *r, char *out, size_t max) {
  const uint8_t length = prv_u8(r);
  if (!r->ok || r->offset + length > r->length) {
    r->ok = false;
    out[0] = '\0';
    return;
  }
  size_t n = length < max ? length : max;
  while (n > 0 && n < length && (r->data[r->offset + n] & 0xC0) == 0x80) {
    n--;
  }
  memcpy(out, r->data + r->offset, n);
  out[n] = '\0';
  r->offset += length;
}

bool watch_actions_parse(const uint8_t *payload, size_t length, WatchActionRequest *out,
                         uint8_t *status_out) {
  Reader r = {.data = payload, .length = length, .ok = true};
  memset(out, 0, sizeof(*out));
  out->kind = prv_u8(&r);
  if (status_out) {
    *status_out = AudioCompanionWatchActionStatusFailed;
  }
  switch (out->kind) {
    case AudioCompanionWatchActionTimer:
      out->timer.seconds = prv_u32(&r);
      prv_string(&r, out->timer.label, AUDIO_COMPANION_WATCH_ACTION_LABEL_MAX_BYTES);
      return r.ok && out->timer.seconds >= 1 && out->timer.seconds <= 86400;
    case AudioCompanionWatchActionAlarm:
      out->alarm.hour = prv_u8(&r);
      out->alarm.minute = prv_u8(&r);
      prv_string(&r, out->alarm.label, AUDIO_COMPANION_WATCH_ACTION_LABEL_MAX_BYTES);
      return r.ok && out->alarm.hour <= 23 && out->alarm.minute <= 59;
    case AudioCompanionWatchActionChoose: {
      const uint16_t timeout = prv_u16(&r);
      out->choose.timeout_s = timeout < 10 ? 10 : (timeout > 300 ? 300 : timeout);
      prv_string(&r, out->choose.question, AUDIO_COMPANION_WATCH_ACTION_QUESTION_MAX_BYTES);
      out->choose.count = prv_u8(&r);
      if (!r.ok || out->choose.count < AUDIO_COMPANION_WATCH_ACTION_MIN_OPTIONS ||
          out->choose.count > AUDIO_COMPANION_WATCH_ACTION_MAX_OPTIONS) {
        return false;
      }
      for (uint8_t i = 0; i < out->choose.count; i++) {
        prv_string(&r, out->choose.options[i], AUDIO_COMPANION_WATCH_ACTION_OPTION_MAX_BYTES);
      }
      return r.ok;
    }
    case AudioCompanionWatchActionMusic:
      out->music.command = prv_u8(&r);
      return r.ok && out->music.command >= AudioCompanionWatchMusicPlay &&
             out->music.command <= AudioCompanionWatchMusicNowPlaying;
    case AudioCompanionWatchActionQuietTime:
      out->quiet_time.on = prv_u8(&r) != 0;
      out->quiet_time.minutes = prv_u16(&r);
      return r.ok;
    case AudioCompanionWatchActionLaunchApp:
      prv_string(&r, out->launch_app.name, AUDIO_COMPANION_WATCH_ACTION_APP_NAME_MAX_BYTES);
      return r.ok && out->launch_app.name[0] != '\0';
    case AudioCompanionWatchActionFindWatch:
      out->find_watch.seconds = prv_u8(&r);
      if (out->find_watch.seconds == 0) {
        out->find_watch.seconds = 10;
      } else if (out->find_watch.seconds > AUDIO_COMPANION_WATCH_ACTION_FIND_MAX_SECONDS) {
        out->find_watch.seconds = AUDIO_COMPANION_WATCH_ACTION_FIND_MAX_SECONDS;
      }
      return r.ok;
    case AudioCompanionWatchActionHealth:
      out->health.metric = prv_u8(&r);
      out->health.days = prv_u8(&r);
      if (out->health.days == 0) {
        out->health.days = 1;
      } else if (out->health.days > AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS) {
        out->health.days = AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS;
      }
      return r.ok && out->health.metric >= AudioCompanionWatchHealthSteps &&
             out->health.metric <= AudioCompanionWatchHealthHeartRateDaily;
    case AudioCompanionWatchActionNotice:
      prv_string(&r, out->notice.text, AUDIO_COMPANION_WATCH_ACTION_NOTICE_MAX_BYTES);
      return r.ok && out->notice.text[0] != '\0';
    default:
      if (status_out) {
        *status_out = AudioCompanionWatchActionStatusUnsupported;
      }
      return false;
  }
}

// ---- timers ----

static void prv_timer_done_kernel_main_cb(void *data) {
  char *label = data;
  if (s_timer_done_handler) {
    s_timer_done_handler(label);
  }
  kernel_free(label);
}

static void prv_timer_fired_cb(void *data) {
  const uintptr_t slot = (uintptr_t)data;
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (slot >= WATCH_ACTIONS_MAX_TIMERS || !s_timers[slot].active) {
    pbl_mutex_unlock(&s_lock);
    return;
  }
  s_timers[slot].active = false;
  char *label = kernel_malloc(sizeof(s_timers[slot].label));
  if (label) {
    memcpy(label, s_timers[slot].label, sizeof(s_timers[slot].label));
  }
  pbl_mutex_unlock(&s_lock);
  PBL_LOG_INFO("Watch action: timer done");
  if (label) {
    launcher_task_add_callback(prv_timer_done_kernel_main_cb, label);
  }
}

static bool prv_start_timer(uint32_t seconds, const char *label) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  int slot = -1;
  for (int i = 0; i < WATCH_ACTIONS_MAX_TIMERS; i++) {
    if (!s_timers[i].active) {
      slot = i;
      break;
    }
  }
  bool ok = false;
  if (slot >= 0) {
    WatchTimer *t = &s_timers[slot];
    if (t->timer == TIMER_INVALID_ID) {
      t->timer = new_timer_create();
    }
    strncpy(t->label, label, sizeof(t->label) - 1);
    t->label[sizeof(t->label) - 1] = '\0';
    t->active = new_timer_start(t->timer, seconds * 1000, prv_timer_fired_cb,
                                (void *)(uintptr_t)slot, 0);
    ok = t->active;
  }
  pbl_mutex_unlock(&s_lock);
  return ok;
}

uint32_t watch_actions_active_timer_count(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  uint32_t count = 0;
  for (int i = 0; i < WATCH_ACTIONS_MAX_TIMERS; i++) {
    count += s_timers[i].active ? 1 : 0;
  }
  pbl_mutex_unlock(&s_lock);
  return count;
}

// ---- A notice ----

static void prv_notice_kernel_main_cb(void *data) {
  char *text = data;
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const WatchActionsNoticeHandler handler = s_notice_handler;
  pbl_mutex_unlock(&s_lock);
  if (handler) {
    handler(text);
  }
  kernel_free(text);
}

// ---- Quiet Time for a while ----

static void prv_quiet_time_off_system_task_cb(void *data) {
  PBL_LOG_INFO("Watch action: Quiet Time over");
  watch_actions_set_quiet_time(false);
}

static void prv_quiet_time_timer_cb(void *data) {
  system_task_add_callback(prv_quiet_time_off_system_task_cb, NULL);
}

static void prv_set_quiet_time(bool on, uint16_t minutes) {
  watch_actions_set_quiet_time(on);
  if (s_quiet_time_timer == TIMER_INVALID_ID) {
    s_quiet_time_timer = new_timer_create();
  }
  if (on && minutes > 0) {
    new_timer_start(s_quiet_time_timer, (uint32_t)minutes * 60 * 1000, prv_quiet_time_timer_cb,
                    NULL, 0);
  } else {
    new_timer_stop(s_quiet_time_timer);
  }
}

// ---- running a complete action (system task, no service lock) ----

static void prv_choose_kernel_main_cb(void *data) {
  PendingAction *pending = data;
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool still_wanted = s_choice_action_id == pending->action_id;
  const WatchActionsChooseHandler handler = s_choose_handler;
  pbl_mutex_unlock(&s_lock);
  if (still_wanted && handler) {
    handler(pending->action_id, &pending->request);
  } else if (still_wanted) {
    watch_actions_choice_result(pending->action_id, AudioCompanionWatchActionStatusUnsupported, 0);
  }
  kernel_free(pending);
}

static void prv_run_system_task_cb(void *data) {
  PendingAction *pending = data;
  const WatchActionRequest *request = &pending->request;
  switch (request->kind) {
    case AudioCompanionWatchActionTimer: {
      const bool ok = prv_start_timer(request->timer.seconds, request->timer.label);
      PBL_LOG_INFO("Watch action %u: timer %lus %s", pending->action_id,
                   (unsigned long)request->timer.seconds, ok ? "set" : "failed");
      audio_companion_send_watch_action_result(
          pending->action_id,
          ok ? AudioCompanionWatchActionStatusDone : AudioCompanionWatchActionStatusFailed, 0,
          NULL);
      kernel_free(pending);
      return;
    }
    case AudioCompanionWatchActionAlarm: {
      const bool ok = watch_actions_create_alarm(request->alarm.hour, request->alarm.minute);
      PBL_LOG_INFO("Watch action %u: alarm %02u:%02u %s", pending->action_id,
                   request->alarm.hour, request->alarm.minute, ok ? "set" : "failed");
      audio_companion_send_watch_action_result(
          pending->action_id,
          ok ? AudioCompanionWatchActionStatusDone : AudioCompanionWatchActionStatusFailed, 0,
          NULL);
      kernel_free(pending);
      return;
    }
    case AudioCompanionWatchActionChoose:
      // The choice is shown on KernelMain; its result comes back through
      // watch_actions_choice_result().
      launcher_task_add_callback(prv_choose_kernel_main_cb, pending);
      return;
    case AudioCompanionWatchActionMusic: {
      char text[AUDIO_COMPANION_WATCH_ACTION_RESULT_TEXT_MAX_BYTES + 1] = {0};
      const bool ok = watch_actions_music(request->music.command, text, sizeof(text));
      PBL_LOG_INFO("Watch action %u: music %u %s", pending->action_id, request->music.command,
                   ok ? "sent" : "failed");
      audio_companion_send_watch_action_result(
          pending->action_id,
          ok ? AudioCompanionWatchActionStatusDone : AudioCompanionWatchActionStatusFailed, 0,
          text);
      kernel_free(pending);
      return;
    }
    case AudioCompanionWatchActionQuietTime:
      prv_set_quiet_time(request->quiet_time.on, request->quiet_time.minutes);
      PBL_LOG_INFO("Watch action %u: Quiet Time %s (%u min)", pending->action_id,
                   request->quiet_time.on ? "on" : "off", request->quiet_time.minutes);
      audio_companion_send_watch_action_result(pending->action_id,
                                               AudioCompanionWatchActionStatusDone, 0, NULL);
      kernel_free(pending);
      return;
    case AudioCompanionWatchActionLaunchApp: {
      char found[AUDIO_COMPANION_WATCH_ACTION_APP_NAME_MAX_BYTES + 1] = {0};
      const bool ok = watch_actions_launch_app(request->launch_app.name, found, sizeof(found));
      PBL_LOG_INFO("Watch action %u: launch %s", pending->action_id, ok ? "found" : "no match");
      audio_companion_send_watch_action_result(
          pending->action_id,
          ok ? AudioCompanionWatchActionStatusDone : AudioCompanionWatchActionStatusFailed, 0,
          ok ? found : NULL);
      kernel_free(pending);
      return;
    }
    case AudioCompanionWatchActionFindWatch:
      watch_actions_find_watch(request->find_watch.seconds);
      audio_companion_send_watch_action_result(pending->action_id,
                                               AudioCompanionWatchActionStatusDone, 0, NULL);
      kernel_free(pending);
      return;
    case AudioCompanionWatchActionNotice: {
      const size_t length = strlen(request->notice.text) + 1;
      char *text = kernel_malloc(length);
      if (text) {
        memcpy(text, request->notice.text, length);
        launcher_task_add_callback(prv_notice_kernel_main_cb, text);
      }
      audio_companion_send_watch_action_result(
          pending->action_id,
          text ? AudioCompanionWatchActionStatusDone : AudioCompanionWatchActionStatusFailed, 0,
          NULL);
      kernel_free(pending);
      return;
    }
    case AudioCompanionWatchActionHealth: {
      if (request->health.metric == AudioCompanionWatchHealthHeartRateDaily) {
        // Read from the minute history a little at a time; the result is sent when it's done.
        watch_actions_heart_rate_daily(pending->action_id, request->health.days);
        kernel_free(pending);
        return;
      }
      char text[AUDIO_COMPANION_WATCH_ACTION_RESULT_TEXT_MAX_BYTES + 1] = {0};
      const bool ok =
          watch_actions_health(request->health.metric, request->health.days, text, sizeof(text));
      PBL_LOG_INFO("Watch action %u: health metric %u %s", pending->action_id,
                   request->health.metric, ok ? "read" : "unavailable");
      audio_companion_send_watch_action_result(
          pending->action_id,
          ok ? AudioCompanionWatchActionStatusDone : AudioCompanionWatchActionStatusFailed, 0,
          (ok || text[0]) ? text : NULL);
      kernel_free(pending);
      return;
    }
    default:
      kernel_free(pending);
      return;
  }
}

// ---- fragments (control dispatch, service lock held) ----

static void prv_reject_system_task_cb(void *data) {
  const uintptr_t packed = (uintptr_t)data;
  audio_companion_send_watch_action_result((uint8_t)(packed >> 8), (uint8_t)(packed & 0xFF), 0,
                                           NULL);
}

uint8_t watch_actions_handle_fragment(uint8_t action_id, uint8_t flags, const uint8_t *data,
                                      uint8_t length) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (action_id != s_assembly_id) {
    s_assembly_id = action_id;  // a new action; whatever was half-assembled is abandoned
    s_assembly_length = 0;
  }
  if (s_assembly_length + length > sizeof(s_assembly)) {
    s_assembly_id = 0;
    s_assembly_length = 0;
    pbl_mutex_unlock(&s_lock);
    return AudioCompanionAckStatusRejected;
  }
  memcpy(s_assembly + s_assembly_length, data, length);
  s_assembly_length += length;
  if (flags & AUDIO_COMPANION_WATCH_ACTION_FLAG_MORE) {
    pbl_mutex_unlock(&s_lock);
    return AudioCompanionAckStatusOk;
  }

  PendingAction *pending = kernel_zalloc(sizeof(*pending));
  uint8_t status = AudioCompanionWatchActionStatusFailed;
  const bool parsed =
      pending && watch_actions_parse(s_assembly, s_assembly_length, &pending->request, &status);
  s_assembly_id = 0;
  s_assembly_length = 0;
  if (!pending) {
    pbl_mutex_unlock(&s_lock);
    return AudioCompanionAckStatusBusy;
  }
  if (!parsed) {
    pbl_mutex_unlock(&s_lock);
    kernel_free(pending);
    PBL_LOG_WRN("Watch action %u: payload rejected", action_id);
    // ACK the write (it arrived intact) and report the outcome like any other action.
    system_task_add_callback(prv_reject_system_task_cb,
                             (void *)(uintptr_t)(((uintptr_t)action_id << 8) | status));
    return AudioCompanionAckStatusOk;
  }
  if (pending->request.kind == AudioCompanionWatchActionChoose) {
    if (s_choice_action_id != 0) {
      pbl_mutex_unlock(&s_lock);
      kernel_free(pending);
      return AudioCompanionAckStatusBusy;
    }
    s_choice_action_id = action_id;
  }
  pending->action_id = action_id;
  pbl_mutex_unlock(&s_lock);
  system_task_add_callback(prv_run_system_task_cb, pending);
  return AudioCompanionAckStatusOk;
}

void watch_actions_choice_result(uint8_t action_id, uint8_t status, uint8_t index) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool current = action_id != 0 && s_choice_action_id == action_id;
  if (current) {
    s_choice_action_id = 0;
  }
  pbl_mutex_unlock(&s_lock);
  if (!current) {
    return;
  }
  PBL_LOG_INFO("Watch action %u: choice %u (%u)", action_id, status, index);
  audio_companion_send_watch_action_result(action_id, status, index, NULL);
}

static void prv_dismiss_kernel_main_cb(void *data) {
  if (s_dismiss_handler) {
    s_dismiss_handler();
  }
}

void watch_actions_receiver_gone(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_assembly_id = 0;
  s_assembly_length = 0;
  const bool had_choice = s_choice_action_id != 0;
  s_choice_action_id = 0;
  pbl_mutex_unlock(&s_lock);
  if (had_choice) {
    launcher_task_add_callback(prv_dismiss_kernel_main_cb, NULL);
  }
}

void watch_actions_set_ui_handlers(WatchActionsChooseHandler choose,
                                   WatchActionsTimerDoneHandler timer_done,
                                   WatchActionsChooseDismissHandler dismiss) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_choose_handler = choose;
  s_timer_done_handler = timer_done;
  s_dismiss_handler = dismiss;
  pbl_mutex_unlock(&s_lock);
}

void watch_actions_set_notice_handler(WatchActionsNoticeHandler notice) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_notice_handler = notice;
  pbl_mutex_unlock(&s_lock);
}

void watch_actions_init(void) {
  for (int i = 0; i < WATCH_ACTIONS_MAX_TIMERS; i++) {
    s_timers[i].timer = TIMER_INVALID_ID;
  }
  s_quiet_time_timer = TIMER_INVALID_ID;
}

void watch_actions_test_reset(void) {
  pbl_mutex_init(&s_lock);
  s_assembly_id = 0;
  s_assembly_length = 0;
  s_choice_action_id = 0;
  s_choose_handler = NULL;
  s_timer_done_handler = NULL;
  s_dismiss_handler = NULL;
  s_notice_handler = NULL;
  memset(s_timers, 0, sizeof(s_timers));
  watch_actions_init();
}
