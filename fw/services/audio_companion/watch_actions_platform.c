/* SPDX-License-Identifier: Apache-2.0 */

#include "watch_actions.h"

#include "pbl/services/alarms/alarm.h"

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
