/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/alarms/alarm.h"
#include "watch_actions.h"

#include <stdio.h>
#include <string.h>

static unsigned prv_days(AlarmId id) {
  bool days[PBL_DAY_PER_WEEK] = {0};
  alarm_get_custom_days(id, days);
  unsigned mask = 0;
  for (unsigned day = 0; day < PBL_DAY_PER_WEEK; day++) {
    mask |= days[day] ? (1u << day) : 0;
  }
  return mask;
}

static int prv_row(AlarmId id, const AlarmInfo *info, bool full, char *text, size_t size) {
  if (full) {
    return snprintf(text, size, "%d%02d%02d%d%d%d%02X%d%d%d\n", id, info->hour,
                    info->minute, info->is_smart, info->enabled, info->kind, prv_days(id),
                    info->sound_enabled, info->vibrate_enabled, info->tone);
  }
  return snprintf(text, size, "%d%02d%02d%d%d%d%02X\n", id, info->hour,
                  info->minute, info->is_smart, info->enabled, info->kind, prv_days(id));
}

bool watch_actions_alarms(const WatchActionRequest *request, uint8_t *id, char *text,
                          size_t text_size) {
  const uint8_t operation = request->alarms.operation;
  const bool full = request->kind == AudioCompanionWatchActionAlarmsFull;
  *id = request->alarms.id;
  text[0] = '\0';
  AlarmInfo info;
  if (operation == 0) {
    size_t used = 0;
    for (AlarmId i = 0; i < 10; i++) {
      if (!alarm_get_info(i, &info)) {
        continue;
      }
      const int n = prv_row(i, &info, full, text + used, text_size - used);
      if (n < 0 || (size_t)n >= text_size - used) {
        return false;
      }
      used += n;
    }
    return true;
  }
  if (operation == 1) {
    bool free_slot = false;
    for (AlarmId i = 0; i < 10; i++) {
      if (!alarm_get_info(i, &info)) {
        free_slot = true;
        break;
      }
    }
    if (!free_slot) {
      snprintf(text, text_size, "full: All ten alarm slots are occupied.");
      return false;
    }
    info = (AlarmInfo){
      .hour = request->alarms.hour,
      .minute = request->alarms.minute,
      .kind = ALARM_KIND_JUST_ONCE,
      .enabled = true,
      .is_smart = request->alarms.smart == 1,
      .vibrate_enabled = true,
    };
    AlarmId created = alarm_create(&info);
    if (created == ALARM_INVALID_ID) {
      return false;
    }
    *id = created;
    if (!full) {
      return alarm_get_info(created, &info);
    }
  }
  if (!alarm_get_info(*id, &info)) {
    snprintf(text, text_size, "not_found: List alarms again.");
    return false;
  }
  if (operation == 3) {
    alarm_delete(*id);
    return !alarm_get_info(*id, &info);
  }
  if (operation == 4) {
    return prv_row(*id, &info, true, text, text_size) > 0;
  }
  const int hour = request->alarms.hour == 255 ? info.hour : request->alarms.hour;
  const int minute = request->alarms.minute == 255 ? info.minute : request->alarms.minute;
  const bool smart = request->alarms.smart == 255 ? info.is_smart : request->alarms.smart == 1;
  const bool enabled = request->alarms.enabled == 255 ? info.enabled : request->alarms.enabled == 1;
  const AlarmKind kind = full && request->alarms.recurrence != 255 ?
      request->alarms.recurrence : info.kind;
  const unsigned mask = full && request->alarms.days != 255 ? request->alarms.days : prv_days(*id);
  const bool sound = full && request->alarms.sound != 255 ? request->alarms.sound : info.sound_enabled;
  const bool vibrate = full && request->alarms.vibrate != 255 ?
      request->alarms.vibrate : info.vibrate_enabled;
  const AlarmTone tone = full && request->alarms.tone != 255 ? request->alarms.tone : info.tone;
  if (hour != info.hour || minute != info.minute) {
    alarm_set_time(*id, hour, minute);
  }
  if (smart != info.is_smart) {
    alarm_set_smart(*id, smart);
  }
  if (kind == ALARM_KIND_CUSTOM && (kind != info.kind || mask != prv_days(*id))) {
    bool days[PBL_DAY_PER_WEEK];
    for (int day = 0; day < PBL_DAY_PER_WEEK; ++day) {
      days[day] = (mask & (1u << day)) != 0;
    }
    alarm_set_custom(*id, days);
  } else if (kind != info.kind) {
    alarm_set_kind(*id, kind);
  }
  if (sound != info.sound_enabled) {
    alarm_set_sound_enabled(*id, sound);
  }
  if (vibrate != info.vibrate_enabled) {
    alarm_set_vibrate_enabled(*id, vibrate);
  }
  if (tone != info.tone) {
    alarm_set_tone(*id, tone);
  }
  // Native setters enable the alarm; restore its prior state unless explicitly changed.
  alarm_set_enabled(*id, enabled);
  const bool ok = alarm_get_info(*id, &info) && info.hour == hour && info.minute == minute &&
      info.is_smart == smart && info.enabled == enabled && info.kind == kind &&
      (kind != ALARM_KIND_CUSTOM || prv_days(*id) == mask) && info.sound_enabled == sound &&
      info.vibrate_enabled == vibrate && info.tone == tone;
  if (ok && full) {
    prv_row(*id, &info, true, text, text_size);
  }
  return ok;
}
