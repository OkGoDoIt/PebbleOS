/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "syscall/syscall_internal.h"

#include "applib/peek_widget_private.h"
#include "kernel/pebble_tasks.h"
#include "process_management/app_manager.h"
#include "process_management/worker_manager.h"
#include "pbl/services/alarms/alarm.h"
#include "pbl/services/peek_widgets.h"
#include "shell/prefs.h"

#include <string.h>

DEFINE_SYSCALL(bool, sys_alarm_get_next_enabled, time_t *timestamp_out) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(timestamp_out, sizeof(*timestamp_out));
  }
  return alarm_get_next_enabled_alarm(timestamp_out);
}

DEFINE_SYSCALL(bool, sys_hrm_manager_is_hrm_present) {
#ifdef CONFIG_SERVICE_HRM
  return true;
#else
  return false;
#endif
}

#ifdef CONFIG_SERVICE_PEEK_WIDGETS
_Static_assert(PEEK_WIDGET_TITLE_MAX_LEN == PEEK_WIDGET_APP_TITLE_MAX_LEN,
               "SDK and service title limits must agree");
_Static_assert(PEEK_WIDGET_SUBTITLE_MAX_LEN == PEEK_WIDGET_APP_SUBTITLE_MAX_LEN,
               "SDK and service subtitle limits must agree");

//! The calling process's UUID, resolved kernel-side so an app can only touch its own widget.
static bool prv_get_calling_process_uuid(Uuid *uuid_out) {
  const PebbleProcessMd *md = (pebble_task_get_current() == PebbleTask_Worker)
      ? worker_manager_get_current_worker_md()
      : app_manager_get_current_app_md();
  if (!md) {
    return false;
  }
  *uuid_out = md->uuid;
  return true;
}
#endif

DEFINE_SYSCALL(PeekWidgetResult, sys_peek_widget_publish, const PeekWidgetPublishArgs *args) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(args, sizeof(*args));
  }
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  PeekWidgetPublishArgs args_copy = *args;
  args_copy.title[PEEK_WIDGET_TITLE_MAX_LEN] = '\0';
  args_copy.subtitle[PEEK_WIDGET_SUBTITLE_MAX_LEN] = '\0';
  if (args_copy.title[0] == '\0') {
    return PEEK_WIDGET_RESULT_INVALID_ARGS;
  }
  if (!quick_view_prefs_get_apps_enabled()) {
    return PEEK_WIDGET_RESULT_DISABLED;
  }
  PeekWidgetAppPublish publish = {
    .icon = args_copy.icon,
    .title = args_copy.title,
    .subtitle = args_copy.subtitle,
    .launch_code = args_copy.launch_code,
    .timeout_s = args_copy.timeout_s,
  };
  if (!prv_get_calling_process_uuid(&publish.owner)) {
    return PEEK_WIDGET_RESULT_INVALID_ARGS;
  }
  return peek_widgets_publish_app_widget(&publish)
      ? PEEK_WIDGET_RESULT_SUCCESS : PEEK_WIDGET_RESULT_INVALID_ARGS;
#else
  return PEEK_WIDGET_RESULT_DISABLED;
#endif
}

DEFINE_SYSCALL(void, sys_peek_widget_withdraw, void) {
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  Uuid uuid;
  if (prv_get_calling_process_uuid(&uuid)) {
    peek_widgets_withdraw_app_widget(&uuid);
  }
#endif
}
