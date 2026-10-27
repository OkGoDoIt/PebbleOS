/* SPDX-License-Identifier: Apache-2.0 */

#include "watch_actions.h"

#include "applib/ui/vibes.h"
#include "kernel/event_loop.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_manager.h"
#include "pbl/drivers/rtc.h"
#include "pbl/services/alarms/alarm.h"
#include "pbl/services/light.h"
#include "pbl/services/music.h"
#include "pbl/services/time.h"
#include "pbl/services/vibe_pattern.h"
#include "pbl/services/audio_companion.h"
#include <pbl/util/time.h>
#include "pbl/services/notifications/do_not_disturb.h"
#ifdef CONFIG_SERVICE_ACTIVITY
#include "pbl/services/activity/activity.h"
#include "pbl/services/activity/activity_algorithm.h"
#include "pbl/services/system_task.h"
#include "pbl/services/time.h"
#include "kernel/pbl_malloc.h"
#include "applib/pbl_std/pbl_std.h"
#include <pbl/util/math.h>
#endif

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

bool watch_actions_create_alarm(uint8_t hour, uint8_t minute) {
  // A one-off alarm in the watch's own alarms, so the wearer can see, edit or delete it there.
  // It vibrates like the watch's other alarms; the label stays with the assistant, since alarms
  // carry none.
  const AlarmInfo info = {
    .hour = hour,
    .minute = minute,
    .kind = ALARM_KIND_JUST_ONCE,
    .enabled = true,
    .vibrate_enabled = true,
  };
  return alarm_create(&info) != ALARM_INVALID_ID;
}

// ---- Music, through the watch's own link to the phone's media service ----

static bool prv_music_command(uint8_t command, MusicCommand *out) {
  switch (command) {
    case AudioCompanionWatchMusicPlay: *out = MusicCommandPlay; return true;
    case AudioCompanionWatchMusicPause: *out = MusicCommandPause; return true;
    case AudioCompanionWatchMusicTogglePlayPause: *out = MusicCommandTogglePlayPause; return true;
    case AudioCompanionWatchMusicNext: *out = MusicCommandNextTrack; return true;
    case AudioCompanionWatchMusicPrevious: *out = MusicCommandPreviousTrack; return true;
    case AudioCompanionWatchMusicVolumeUp: *out = MusicCommandVolumeUp; return true;
    case AudioCompanionWatchMusicVolumeDown: *out = MusicCommandVolumeDown; return true;
    default: return false;
  }
}

bool watch_actions_music(uint8_t command, char *text, size_t text_size) {
  MusicCommand music_command;
  bool ok = true;
  if (prv_music_command(command, &music_command)) {
    if (!music_is_command_supported(music_command) &&
        (music_command == MusicCommandPlay || music_command == MusicCommandPause) &&
        music_is_command_supported(MusicCommandTogglePlayPause)) {
      // Some players only take a toggle; ask for it when the state says it will do the right
      // thing.
      const MusicPlayState state = music_get_playback_state();
      const bool playing = state == MusicPlayStatePlaying;
      if (state != MusicPlayStatePlaying && state != MusicPlayStatePaused) {
        snprintf(text, text_size, "state_unknown: Player only supports toggle; no command sent.");
        return false;
      }
      if ((music_command == MusicCommandPlay) == playing) {
        return true;
      }
      if ((music_command == MusicCommandPlay) != playing) {
        music_command = MusicCommandTogglePlayPause;
      }
    }
    ok = music_is_command_supported(music_command);
    if (ok) {
      music_command_send(music_command);
    }
  } else {
    ok = music_has_now_playing();
  }
  if (text && text_size > 0) {
    text[0] = '\0';
    if (music_has_now_playing()) {
      char title[MUSIC_BUFFER_LENGTH];
      char artist[MUSIC_BUFFER_LENGTH];
      music_get_now_playing(title, artist, NULL);
      snprintf(text, text_size, "%s\n%s", title, artist);
    }
  }
  return ok;
}

// ---- Quiet Time ----

void watch_actions_set_quiet_time(bool on) {
  do_not_disturb_set_manually_enabled(on);
}

// ---- Opening a watch app by name ----

typedef struct {
  const char *name;
  //! 3 exact, 2 starts with, 1 contains; ties require the caller to choose.
  bool tied;
  int best_rank;
  AppInstallId best_id;
  char best_name[APP_NAME_SIZE_BYTES];
} AppSearch;

static bool prv_equal_fold(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
      return false;
    }
  }
  return true;
}

static int prv_match_rank(const char *app_name, const char *wanted) {
  const size_t app_len = strlen(app_name);
  const size_t wanted_len = strlen(wanted);
  if (wanted_len == 0 || wanted_len > app_len) {
    return 0;
  }
  if (app_len == wanted_len && prv_equal_fold(app_name, wanted, app_len)) {
    return 3;
  }
  if (prv_equal_fold(app_name, wanted, wanted_len)) {
    return 2;
  }
  for (size_t i = 1; i + wanted_len <= app_len; i++) {
    if (prv_equal_fold(app_name + i, wanted, wanted_len)) {
      return 1;
    }
  }
  return 0;
}

static bool prv_app_search_cb(AppInstallEntry *entry, void *data) {
  AppSearch *search = data;
  if (app_install_entry_is_hidden(entry) || app_install_entry_is_watchface(entry)) {
    return true;
  }
  const int rank = prv_match_rank(entry->name, search->name);
  if (rank > search->best_rank) {
    search->tied = false;
    search->best_rank = rank;
    search->best_id = entry->install_id;
    strncpy(search->best_name, entry->name, sizeof(search->best_name) - 1);
    search->best_name[sizeof(search->best_name) - 1] = '\0';
  }
  else if (rank > 0 && rank == search->best_rank) {
    search->tied = true;
  }
  return true;  // keep looking: an exact match may come later
}

bool watch_actions_launch_app(const char *name, char *found, size_t found_size) {
  AppSearch search = {.name = name, .best_id = INSTALL_ID_INVALID};
  app_install_enumerate_entries(prv_app_search_cb, &search);
  if (search.tied || search.best_rank == 0 || search.best_id == INSTALL_ID_INVALID) {
    snprintf(found, found_size, "%s: List apps and choose an id.", search.tied ? "ambiguous" : "not_found");
    return false;
  }
  app_manager_put_launch_app_event(&(AppLaunchEventConfig){
    .id = search.best_id,
    .common.reason = APP_LAUNCH_USER,
  });
  if (found && found_size > 0) {
    strncpy(found, search.best_name, found_size - 1);
    found[found_size - 1] = '\0';
  }
  return true;
}

// ---- Find my watch ----

#define FIND_PULSE_MS (600)
#define FIND_GAP_MS (400)

static uint32_t s_find_segments[2 * AUDIO_COMPANION_WATCH_ACTION_FIND_MAX_SECONDS];

static void prv_find_kernel_main_cb(void *data) {
  const uint32_t seconds = (uint32_t)(uintptr_t)data;
  if (seconds == 0) {
    vibe_pattern_clear_for_owner(VibePatternOwner_MuseFind);
    return;
  }
  if (seconds > AUDIO_COMPANION_WATCH_ACTION_FIND_MAX_SECONDS) {
    return;
  }
  for (uint32_t i = 0; i < seconds; i++) {
    s_find_segments[2 * i] = FIND_PULSE_MS;
    s_find_segments[2 * i + 1] = FIND_GAP_MS;
  }
  vibes_cancel();
  vibe_pattern_set_owner(VibePatternOwner_MuseFind);
  vibes_enqueue_custom_pattern((VibePattern){
    .durations = s_find_segments,
    .num_segments = 2 * seconds - 1,
  });
  light_enable_interaction();
}

void watch_actions_find_watch(uint8_t seconds) {
  launcher_task_add_callback(prv_find_kernel_main_cb, (void *)(uintptr_t)seconds);
}

// ---- Status and installed apps ----

uint32_t watch_actions_quiet_revision(void) {
  return do_not_disturb_manual_revision();
}

bool watch_actions_inspect(uint8_t section, char *text, size_t size) {
  if (section == 0) {
    time_t now = rtc_get_time();
    struct pbl_tm local;
    pbl_localtime_r(&now, &local);
    // Tool revision 1 includes NativeTimers, AlarmsFull, Inspect, Apps, and FindControl.
    snprintf(text, size, "1,%" PRId32 ",%d,%d\n%s", (int32_t)now, local.tm_gmtoff,
             audio_companion_get_state(), local.tm_zone);
  } else if (section == 1) {
    snprintf(text, size, "%u,%" PRId32, do_not_disturb_source_bits(),
             (int32_t)watch_actions_quiet_until());
  } else if (section == 2) {
    char title[MUSIC_BUFFER_LENGTH] = {0}, artist[MUSIC_BUFFER_LENGTH] = {0};
    if (music_has_now_playing()) {
      music_get_now_playing(title, artist, NULL);
    }
    snprintf(text, size, "%d,%d,%d\n%s\n%s", music_has_now_playing(),
             music_is_playback_state_reporting_supported() ? (int)music_get_playback_state() : -1,
             music_is_volume_reporting_supported() ? music_get_volume_percent() : -1, title, artist);
  } else {
    return false;
  }
  return true;
}

typedef struct {
  unsigned offset;
  unsigned seen;
  unsigned count;
  char *text;
  size_t size;
} AppPage;

static bool prv_app_page_cb(AppInstallEntry *entry, void *data) {
  AppPage *page = data;
  if (app_install_entry_is_hidden(entry) || app_install_entry_is_watchface(entry)) {
    return true;
  }
  if (page->seen++ < page->offset) {
    return true;
  }
  char uuid[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(&entry->uuid, uuid);
  const size_t used = strlen(page->text);
  const int needed = snprintf(NULL, 0, "%s,%s\n", uuid, entry->name);
  if (needed < 0 || used + needed >= page->size) {
    return false;
  }
  snprintf(page->text + used, page->size - used, "%s,%s\n", uuid, entry->name);
  page->count++;
  return true;
}

bool watch_actions_apps(const WatchActionRequest *request, char *text, size_t size) {
  if (request->apps.operation == 0) {
    AppPage page = {.offset = request->apps.offset, .text = text, .size = size};
    text[0] = '\0';
    app_install_enumerate_entries(prv_app_page_cb, &page);
    return true; // An empty page is the explicit end of the list.
  }
  const AppInstallId id = app_install_get_id_for_uuid(&request->apps.uuid);
  AppInstallEntry entry;
  if (id == INSTALL_ID_INVALID || !app_install_get_entry_for_install_id(id, &entry) ||
      app_install_entry_is_hidden(&entry) || app_install_entry_is_watchface(&entry)) {
    snprintf(text, size, "not_found: App is no longer installed.");
    return false;
  }
  app_manager_put_launch_app_event(&(AppLaunchEventConfig){.id = id, .common.reason = APP_LAUNCH_USER});
  snprintf(text, size, "%s", entry.name);
  return true;
}

// ---- Health, from the watch's own activity tracking ----

bool watch_actions_health(uint8_t metric, uint8_t days, char *text, size_t text_size) {
#ifdef CONFIG_SERVICE_ACTIVITY
  if (!text || text_size == 0) {
    return false;
  }
  text[0] = '\0';
  if (!activity_tracking_on()) {
    snprintf(text, text_size, "health_off: Activity tracking is disabled.");
    return false;
  }
  if (metric == AudioCompanionWatchHealthHeartRate) {
    int32_t bpm = -1;
    int32_t updated_utc = 0;
    if (!activity_prefs_heart_rate_is_enabled()) {
      snprintf(text, text_size, "%s", AUDIO_COMPANION_WATCH_HEALTH_HEART_RATE_OFF);
      return false;
    }
    if (!activity_get_metric(ActivityMetricHeartRateFilteredBPM, 1, &bpm) || bpm <= 0) {
      snprintf(text, text_size, "no_data: No heart rate sample is available.");
      return false;
    }
    activity_get_metric(ActivityMetricHeartRateFilteredUpdatedTimeUTC, 1, &updated_utc);
    const time_t now = rtc_get_time();
    const int32_t age = updated_utc > 0 && now >= updated_utc
                            ? (int32_t)(now - updated_utc) : -1;
    snprintf(text, text_size, "%" PRId32 ",%" PRId32, bpm, age);
    return true;
  }
  ActivityMetric activity_metric;
  switch (metric) {
    case AudioCompanionWatchHealthSteps:
      activity_metric = ActivityMetricStepCount;
      break;
    case AudioCompanionWatchHealthDistanceMeters:
      activity_metric = ActivityMetricDistanceMeters;
      break;
    case AudioCompanionWatchHealthActiveSeconds:
      activity_metric = ActivityMetricActiveSeconds;
      break;
    case AudioCompanionWatchHealthActiveKCalories:
      activity_metric = ActivityMetricActiveKCalories;
      break;
    case AudioCompanionWatchHealthSleepSeconds:
      activity_metric = ActivityMetricSleepTotalSeconds;
      break;
    default: return false;
  }
  int32_t values[AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS];
  if (days == 0 || days > AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS ||
      !activity_get_metric(activity_metric, days, values)) {
    snprintf(text, text_size, "no_data: No activity history is available.");
    return false;
  }
  size_t used = 0;
  for (uint8_t i = 0; i < days && used < text_size; i++) {
    const int written = snprintf(text + used, text_size - used, i ? ",%" PRId32 : "%" PRId32,
                                 values[i]);
    if (written < 0) {
      break;
    }
    used += (size_t)written;
  }
  return true;
#else
  if (text && text_size) {
    snprintf(text, text_size, "unsupported: This firmware has no activity service.");
  }
  return false;
#endif
}

// ---- Heart rate by day, from the minute history ----

#ifdef CONFIG_SERVICE_ACTIVITY
//! Minutes read per pass: the system task also drains audio, so a pass stays short.
#define HR_SLICE_MINUTES (60)

typedef struct {
  uint8_t action_id;
  uint8_t days;
  uint8_t day;
  bool any;
  time_t day_start;
  time_t day_end;
  time_t cursor;
  uint32_t sum;
  uint16_t count;
  uint8_t low;
  uint8_t high;
  int16_t average[AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS];
  uint8_t lows[AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS];
  uint8_t highs[AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS];
  HealthMinuteData minutes[HR_SLICE_MINUTES];
} HeartRateDays;

static void prv_hr_start_day(HeartRateDays *job) {
  const time_t today = time_start_of_today();
  job->day_start = today;
  job->day_end = rtc_get_time();
  for (unsigned day = 0; day < job->day; ++day) {
    job->day_end = job->day_start;
    job->day_start = time_util_get_midnight_of(job->day_start - 1);
  }
  job->cursor = job->day_start;
  job->sum = 0;
  job->count = 0;
  job->low = UINT8_MAX;
  job->high = 0;
}

static void prv_hr_finish(HeartRateDays *job) {
  char text[AUDIO_COMPANION_WATCH_ACTION_RESULT_TEXT_MAX_BYTES + 1] = {0};
  size_t used = 0;
  for (uint8_t i = 0; i < job->days && used < sizeof(text); i++) {
    const int written =
        job->average[i] < 0
            ? snprintf(text + used, sizeof(text) - used, i ? ",-1" : "-1")
            : snprintf(text + used, sizeof(text) - used, i ? ",%d/%u/%u" : "%d/%u/%u",
                       job->average[i], job->lows[i], job->highs[i]);
    if (written < 0) {
      break;
    }
    used += (size_t)written;
  }
  PBL_LOG_INFO("Watch action %u: heart rate for %u day(s) %s", job->action_id, job->days,
               job->any ? "read" : "unavailable");
  audio_companion_send_watch_action_result(
      job->action_id, AudioCompanionWatchActionStatusDone, 0, text);
  kernel_free(job);
}

static void prv_hr_step_system_task_cb(void *data) {
  HeartRateDays *job = data;
  if (job->cursor < job->day_end) {
    uint32_t count = (uint32_t)((job->day_end - job->cursor) / SECONDS_PER_MINUTE);
    count = MAX(1, MIN(count, HR_SLICE_MINUTES));
    const time_t asked = job->cursor;
    time_t start = asked;
    uint32_t got = count;
    if (activity_algorithm_get_minute_history(job->minutes, &got, &start) && got > 0) {
      for (uint32_t i = 0; i < got && i < HR_SLICE_MINUTES; i++) {
        const uint8_t bpm = job->minutes[i].heart_rate_bpm;
        if (job->minutes[i].is_invalid || bpm == 0) {
          continue;
        }
        job->sum += bpm;
        job->count++;
        job->low = MIN(job->low, bpm);
        job->high = MAX(job->high, bpm);
      }
      job->cursor = start + (time_t)got * SECONDS_PER_MINUTE;
    }
    // Always forward, even when the history had nothing for the slice.
    job->cursor = MAX(job->cursor, asked + (time_t)count * SECONDS_PER_MINUTE);
    system_task_add_callback(prv_hr_step_system_task_cb, job);
    return;
  }
  if (job->count) {
    job->average[job->day] = (int16_t)((job->sum + job->count / 2) / job->count);
    job->lows[job->day] = job->low;
    job->highs[job->day] = job->high;
    job->any = true;
  } else {
    job->average[job->day] = -1;
  }
  if (++job->day < job->days) {
    prv_hr_start_day(job);
    system_task_add_callback(prv_hr_step_system_task_cb, job);
    return;
  }
  prv_hr_finish(job);
}
#endif

void watch_actions_heart_rate_daily(uint8_t action_id, uint8_t days) {
#ifdef CONFIG_SERVICE_ACTIVITY
  if (!activity_prefs_heart_rate_is_enabled()) {
    audio_companion_send_watch_action_result(action_id, AudioCompanionWatchActionStatusFailed, 0,
                                             AUDIO_COMPANION_WATCH_HEALTH_HEART_RATE_OFF);
    return;
  }
  if (!activity_tracking_on()) {
    audio_companion_send_watch_action_result(action_id, AudioCompanionWatchActionStatusFailed, 0,
                                             "health_off: Activity tracking is disabled.");
    return;
  }
  HeartRateDays *job = kernel_zalloc(sizeof(*job));
  if (!job) {
    audio_companion_send_watch_action_result(action_id, AudioCompanionWatchActionStatusFailed, 0,
                                             "busy: Health history could not be allocated.");
    return;
  }
  job->action_id = action_id;
  job->days = MAX(1, MIN(days, AUDIO_COMPANION_WATCH_ACTION_HEALTH_MAX_DAYS));
  prv_hr_start_day(job);
  system_task_add_callback(prv_hr_step_system_task_cb, job);
#else
  audio_companion_send_watch_action_result(action_id, AudioCompanionWatchActionStatusUnsupported, 0,
                                           "unsupported: This firmware has no activity service.");
#endif
}
