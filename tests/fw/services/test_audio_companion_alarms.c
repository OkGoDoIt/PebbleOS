/* SPDX-License-Identifier: Apache-2.0 */
#include "clar.h"
#include "pbl/services/alarms/alarm.h"
#include "services/audio_companion/watch_actions.h"

#include <string.h>

static AlarmInfo s_alarms[10];
static bool s_exists[10];
static bool s_days[10][PBL_DAY_PER_WEEK];

bool alarm_get_info(AlarmId id, AlarmInfo *info) {
  if (id < 0 || id >= 10 || !s_exists[id])
    return false;
  *info = s_alarms[id];
  return true;
}
bool alarm_get_custom_days(AlarmId id, bool days[PBL_DAY_PER_WEEK]) {
  memcpy(days, s_days[id], sizeof(s_days[id]));
  return s_exists[id];
}
AlarmId alarm_create(const AlarmInfo *info) {
  for (int i = 0; i < 10; i++) {
    if (!s_exists[i]) {
      s_exists[i] = true;
      s_alarms[i] = *info;
      return i;
    }
  }
  cl_assert(false);
  return ALARM_INVALID_ID;
}
void alarm_set_time(AlarmId id, int hour, int minute) {
  s_alarms[id].hour = hour;
  s_alarms[id].minute = minute;
  s_alarms[id].enabled = true;
}
void alarm_set_smart(AlarmId id, bool smart) {
  s_alarms[id].is_smart = smart;
  s_alarms[id].enabled = true;
}
void alarm_set_enabled(AlarmId id, bool enabled) {
  s_alarms[id].enabled = enabled;
}
void alarm_delete(AlarmId id) {
  s_exists[id] = false;
}

void test_audio_companion_alarms__initialize(void) {
  memset(s_exists, 0, sizeof(s_exists));
  memset(s_alarms, 0, sizeof(s_alarms));
  memset(s_days, 0, sizeof(s_days));
}

static bool run(uint8_t op, uint8_t slot, uint8_t hour, uint8_t minute, uint8_t smart,
                uint8_t enabled, uint8_t *id, char *text) {
  WatchActionRequest request = {
    .kind = AudioCompanionWatchActionAlarms,
    .alarms = {
      .operation = op,
      .id = slot,
      .hour = hour,
      .minute = minute,
      .smart = smart,
      .enabled = enabled
    }
  };
  return watch_actions_alarms(&request, id, text, 161);
}

void test_audio_companion_alarms__create_edit_list_delete_smart(void) {
  uint8_t id;
  char text[161];
  cl_assert(run(1, 0, 11, 18, 1, 1, &id, text));
  cl_assert_equal_i(id, 0);
  cl_assert(s_alarms[0].is_smart && s_alarms[0].enabled && s_alarms[0].vibrate_enabled);
  cl_assert_equal_i(s_alarms[0].kind, ALARM_KIND_JUST_ONCE);
  cl_assert(run(2, 0, 255, 30, 255, 255, &id, text));
  cl_assert_equal_i(s_alarms[0].hour, 11);
  cl_assert_equal_i(s_alarms[0].minute, 30);
  cl_assert(s_alarms[0].is_smart);
  cl_assert(run(0, 0, 255, 255, 255, 255, &id, text));
  cl_assert_equal_s(text, "0113011300\n");
  cl_assert(run(3, 0, 255, 255, 255, 255, &id, text));
  cl_assert(!s_exists[0]);
  cl_assert(!run(2, 0, 7, 0, 255, 255, &id, text));
  cl_assert(!run(3, 0, 255, 255, 255, 255, &id, text));
}

void test_audio_companion_alarms__edit_preserves_disabled_recurring_and_sound(void) {
  uint8_t id;
  char text[161];
  s_exists[3] = true;
  s_alarms[3] = (AlarmInfo){
    .hour = 7,
    .minute = 30,
    .kind = ALARM_KIND_CUSTOM,
    .enabled = false,
    .is_smart = true,
    .sound_enabled = true,
    .tone = AlarmTone_Chime
  };
  s_days[3][1] = true;
  cl_assert(run(2, 3, 8, 255, 0, 255, &id, text));
  cl_assert_equal_i(s_alarms[3].hour, 8);
  cl_assert_equal_i(s_alarms[3].minute, 30);
  cl_assert(!s_alarms[3].enabled && !s_alarms[3].is_smart);
  cl_assert(s_alarms[3].sound_enabled && s_days[3][1]);
  cl_assert_equal_i(s_alarms[3].kind, ALARM_KIND_CUSTOM);
  cl_assert_equal_i(s_alarms[3].tone, AlarmTone_Chime);
}

void test_audio_companion_alarms__ten_alarms_fit_and_full_create_fails(void) {
  uint8_t id;
  char text[161];
  for (int i = 0; i < 10; i++) {
    cl_assert(run(1, 0, 23, 59, i % 2, 1, &id, text));
    cl_assert_equal_i(id, i);
  }
  cl_assert(!run(1, 0, 12, 0, 1, 1, &id, text));
  cl_assert(run(0, 0, 255, 255, 255, 255, &id, text));
  cl_assert_equal_i(strlen(text), 110);
}
