/* SPDX-License-Identifier: Apache-2.0 */
#include <clar.h>
#include <string.h>
#include "services/audio_companion/watch_actions.h"
#include <pbl/services/music.h>
#include <pbl/services/alarms/alarm.h>
#include <pbl/services/time.h>
#include <pbl/util/time.h>
#include <pbl/services/vibe_pattern.h>
#include <process_management/app_install_manager.h>
#include <process_management/app_manager.h>
#include <applib/ui/vibes.h>

static AppInstallEntry s_apps[4];
static int s_launched, s_sent, s_pulses;
static MusicPlayState s_state;
static VibePatternOwner s_owner, s_cleared;

void app_install_enumerate_entries(AppInstallEnumerateCb cb, void *data) {
  for (int i = 0; i < 4; i++) {
    if (!cb(&s_apps[i], data)) break;
  }
}
bool app_install_entry_is_hidden(const AppInstallEntry *entry) { return false; }
bool app_install_entry_is_watchface(const AppInstallEntry *entry) { return false; }
void app_manager_put_launch_app_event(const AppLaunchEventConfig *config) { s_launched = config->id; }
bool music_is_command_supported(MusicCommand command) { return command == MusicCommandTogglePlayPause; }
MusicPlayState music_get_playback_state(void) { return s_state; }
void music_command_send(MusicCommand command) { s_sent++; }
bool music_has_now_playing(void) { return false; }
void launcher_task_add_callback(void (*cb)(void *), void *data) { cb(data); }
void vibes_cancel(void) {}
void vibe_pattern_set_owner(VibePatternOwner owner) { s_owner = owner; }
void vibe_pattern_clear_for_owner(VibePatternOwner owner) { s_cleared = owner; }
void vibes_enqueue_custom_pattern(VibePattern pattern) { s_pulses = pattern.num_segments; }
time_t watch_actions_quiet_until(void) { return 0; }

void test_audio_companion_platform__initialize(void) {
  s_launched = -1; s_sent = 0; s_pulses = 0;
  s_owner = s_cleared = VibePatternOwner_Other;
  s_state = MusicPlayStateUnknown;
  memset(s_apps, 0, sizeof(s_apps));
  const char *names[] = {"Timer", "Timers", "Workout", "Weather"};
  for (int i = 0; i < 4; i++) {
    strcpy(s_apps[i].name, names[i]);
    s_apps[i].install_id = i + 1;
    s_apps[i].uuid.byte15 = i + 1;
  }
}

void test_audio_companion_platform__ambiguous_names_never_launch_and_pages_preserve_ids(void) {
  char text[161];
  cl_assert(!watch_actions_launch_app("Tim", text, sizeof(text)));
  cl_assert_equal_i(s_launched, -1);
  cl_assert(strstr(text, "ambiguous:") == text);
  cl_assert(watch_actions_launch_app("Timer", text, sizeof(text)));
  cl_assert_equal_i(s_launched, 1);
  WatchActionRequest request = {.kind = AudioCompanionWatchActionApps};
  cl_assert(watch_actions_apps(&request, text, sizeof(text)));
  cl_assert(strstr(text, "000000000001},Timer\n") != NULL);
  request.apps.offset = 3;
  cl_assert(watch_actions_apps(&request, text, sizeof(text)));
  cl_assert(strstr(text, "000000000004},Weather\n") != NULL);
  request.apps.offset = 4;
  cl_assert(watch_actions_apps(&request, text, sizeof(text)));
  cl_assert_equal_s(text, "");
}

void test_audio_companion_platform__music_toggle_requires_known_state_and_skips_noop(void) {
  char text[161];
  cl_assert(!watch_actions_music(AudioCompanionWatchMusicPlay, text, sizeof(text)));
  cl_assert_equal_i(s_sent, 0);
  s_state = MusicPlayStatePlaying;
  cl_assert(watch_actions_music(AudioCompanionWatchMusicPlay, text, sizeof(text)));
  cl_assert_equal_i(s_sent, 0);
  cl_assert(watch_actions_music(AudioCompanionWatchMusicPause, text, sizeof(text)));
  cl_assert_equal_i(s_sent, 1);
}

void test_audio_companion_platform__find_stop_is_scoped_and_zero_never_underflows(void) {
  watch_actions_find_watch(10);
  cl_assert_equal_i(s_pulses, 19);
  cl_assert_equal_i(s_owner, VibePatternOwner_MuseFind);
  watch_actions_find_watch(0);
  cl_assert_equal_i(s_cleared, VibePatternOwner_MuseFind);
  cl_assert_equal_i(s_pulses, 19);
  watch_actions_find_watch(255);
  cl_assert_equal_i(s_pulses, 19);
}

void test_audio_companion_platform__missing_activity_service_is_explicitly_unsupported(void) {
  char text[161];
  cl_assert(!watch_actions_health(AudioCompanionWatchHealthSteps, 1, text, sizeof(text)));
  cl_assert(strstr(text, "unsupported:") == text);
}

AlarmId alarm_create(const AlarmInfo *info) { return ALARM_INVALID_ID; }
AudioCompanionServiceState audio_companion_get_state(void) { return AudioCompanionServiceStateIdle; }
bool audio_companion_send_watch_action_result(uint8_t id, uint8_t status, uint8_t value, const char *text) { return true; }
uint32_t do_not_disturb_manual_revision(void) { return 0; }
void do_not_disturb_set_manually_enabled(bool enabled) {}
uint8_t do_not_disturb_source_bits(void) { return 0; }
uint8_t music_get_volume_percent(void) { return 60; }
bool music_is_playback_state_reporting_supported(void) { return true; }
bool music_is_volume_reporting_supported(void) { return true; }
struct pbl_tm *pbl_localtime_r(const time_t *t, struct pbl_tm *result) {
  memset(result, 0, sizeof(*result));
  return result;
}
