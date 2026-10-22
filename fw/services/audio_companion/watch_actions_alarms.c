/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/alarms/alarm.h"
#include "watch_actions.h"

#include <stdio.h>
#include <string.h>

bool watch_actions_alarms(const WatchActionRequest *request, uint8_t *id, char *text,
                          size_t text_size) {
  const uint8_t operation = request->alarms.operation;
  *id = request->alarms.id;
  text[0] = '\0';
  AlarmInfo info;
  if (operation == 0) {
    size_t used = 0;
    for (AlarmId i = 0; i < 10; i++) {
      if (!alarm_get_info(i, &info)) {
        continue;
      }
      bool days[PBL_DAY_PER_WEEK] = {0};
      alarm_get_custom_days(i, days);
      unsigned mask = 0;
      for (unsigned day = 0; day < PBL_DAY_PER_WEEK; day++) {
        mask |= days[day] ? (1u << day) : 0;
      }
      const int n = snprintf(text + used, text_size - used, "%d%02d%02d%d%d%d%02X\n", i, info.hour,
                             info.minute, info.is_smart, info.enabled, info.kind, mask);
      if (n < 0 || (size_t)n >= text_size - used) {
        return false;
      }
      used += n;
    }
    return true;
  }
  if (operation == 1) {
    // alarm_create assumes a free slot. Refuse a full watch before calling it.
    bool free_slot = false;
    for (AlarmId i = 0; i < 10; i++) {
      if (!alarm_get_info(i, &info)) {
        free_slot = true;
        break;
      }
    }
    if (!free_slot) {
      return false;
    }
    info = (AlarmInfo){
      .hour = request->alarms.hour,
      .minute = request->alarms.minute,
      .kind = ALARM_KIND_JUST_ONCE,
      .enabled = true,
      .is_smart = request->alarms.smart == 1,
      .vibrate_enabled = true
    };
    AlarmId created = alarm_create(&info);
    if (created == ALARM_INVALID_ID) {
      return false;
    }
    *id = created;
    return alarm_get_info(created, &info);
  }
  if (!alarm_get_info(*id, &info)) {
    return false;
  }
  if (operation == 3) {
    alarm_delete(*id);
    return !alarm_get_info(*id, &info);
  }
  const int hour = request->alarms.hour == 255 ? info.hour : request->alarms.hour;
  const int minute = request->alarms.minute == 255 ? info.minute : request->alarms.minute;
  const bool smart = request->alarms.smart == 255 ? info.is_smart : request->alarms.smart == 1;
  const bool enabled = request->alarms.enabled == 255 ? info.enabled : request->alarms.enabled == 1;
  if (hour != info.hour || minute != info.minute) {
    alarm_set_time(*id, hour, minute);
  }
  if (smart != info.is_smart) {
    alarm_set_smart(*id, smart);
  }
  // Native setters enable the alarm; restore its prior state unless explicitly changed.
  alarm_set_enabled(*id, enabled);
  return alarm_get_info(*id, &info) && info.hour == hour && info.minute == minute &&
         info.is_smart == smart && info.enabled == enabled;
}
