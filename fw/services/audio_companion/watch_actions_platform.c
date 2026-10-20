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
#include "pbl/services/notifications/do_not_disturb.h"
#ifdef CONFIG_SERVICE_ACTIVITY
#include "pbl/services/activity/activity.h"
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
  //! 3 exact, 2 starts with, 1 contains; the best match wins, the first among equals.
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
    search->best_rank = rank;
    search->best_id = entry->install_id;
    strncpy(search->best_name, entry->name, sizeof(search->best_name) - 1);
    search->best_name[sizeof(search->best_name) - 1] = '\0';
  }
  return true;  // keep looking: an exact match may come later
}

bool watch_actions_launch_app(const char *name, char *found, size_t found_size) {
  AppSearch search = {.name = name, .best_id = INSTALL_ID_INVALID};
  app_install_enumerate_entries(prv_app_search_cb, &search);
  if (search.best_rank == 0 || search.best_id == INSTALL_ID_INVALID) {
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
  for (uint32_t i = 0; i < seconds; i++) {
    s_find_segments[2 * i] = FIND_PULSE_MS;
    s_find_segments[2 * i + 1] = FIND_GAP_MS;
  }
  vibes_cancel();
  vibes_enqueue_custom_pattern((VibePattern){
    .durations = s_find_segments,
    .num_segments = 2 * seconds - 1,
  });
  light_enable_interaction();
}

void watch_actions_find_watch(uint8_t seconds) {
  launcher_task_add_callback(prv_find_kernel_main_cb, (void *)(uintptr_t)seconds);
}

// ---- Health, from the watch's own activity tracking ----

bool watch_actions_health(uint8_t metric, uint8_t days, char *text, size_t text_size) {
#ifdef CONFIG_SERVICE_ACTIVITY
  if (!text || text_size == 0) {
    return false;
  }
  text[0] = '\0';
  if (metric == AudioCompanionWatchHealthHeartRate) {
    int32_t bpm = -1;
    int32_t updated_utc = 0;
    if (!activity_get_metric(ActivityMetricHeartRateFilteredBPM, 1, &bpm) || bpm <= 0) {
      return false;
    }
    activity_get_metric(ActivityMetricHeartRateFilteredUpdatedTimeUTC, 1, &updated_utc);
    const int32_t age = updated_utc > 0 ? (int32_t)(rtc_get_time() - updated_utc) : -1;
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
  return false;
#endif
}
