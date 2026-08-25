/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "applib/peek_widget_private.h"
#include "kernel/pebble_tasks.h"
#include "process_management/pebble_process_md.h"
#include "pbl/services/peek_widgets.h"
#include "pbl/util/size.h"
#include "syscall/syscall.h"

#include <string.h>

static const Uuid s_app_uuid = {
  0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
};

static const Uuid s_worker_uuid = {
  0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b,
};

static PebbleTask s_current_task;
static PebbleProcessMd s_app_md;
static PebbleProcessMd s_worker_md;

static bool s_apps_enabled;
static bool s_publish_accepted;
static PeekWidgetAppPublish s_last_publish;
static char s_last_publish_title[PEEK_WIDGET_TITLE_MAX_LEN + 1];
static char s_last_publish_subtitle[PEEK_WIDGET_SUBTITLE_MAX_LEN + 1];
static unsigned int s_num_publishes;
static Uuid s_last_withdraw_owner;
static unsigned int s_num_withdraws;

// Fakes
////////////////////////////////////////////////////////////////

bool alarm_get_next_enabled_alarm(time_t *timestamp_out) {
  return false;
}

PebbleTask pebble_task_get_current(void) {
  return s_current_task;
}

const PebbleProcessMd *app_manager_get_current_app_md(void) {
  return &s_app_md;
}

const PebbleProcessMd *worker_manager_get_current_worker_md(void) {
  return &s_worker_md;
}

bool quick_view_prefs_get_apps_enabled(void) {
  return s_apps_enabled;
}

bool peek_widgets_publish_app_widget(const PeekWidgetAppPublish *publish) {
  s_num_publishes++;
  s_last_publish = *publish;
  // The string pointers do not outlive the call; snapshot them.
  strncpy(s_last_publish_title, publish->title, PEEK_WIDGET_TITLE_MAX_LEN);
  strncpy(s_last_publish_subtitle, publish->subtitle ?: "", PEEK_WIDGET_SUBTITLE_MAX_LEN);
  return s_publish_accepted;
}

void peek_widgets_withdraw_app_widget(const Uuid *owner) {
  s_num_withdraws++;
  s_last_withdraw_owner = *owner;
}

// Setup
////////////////////////////////////////////////////////////////

void test_peek_widget__initialize(void) {
  s_current_task = PebbleTask_App;
  s_app_md = (PebbleProcessMd) { .uuid = s_app_uuid };
  s_worker_md = (PebbleProcessMd) { .uuid = s_worker_uuid };
  s_apps_enabled = true;
  s_publish_accepted = true;
  s_num_publishes = 0;
  s_num_withdraws = 0;
  s_last_publish = (PeekWidgetAppPublish) {};
  s_last_publish_title[0] = '\0';
  s_last_publish_subtitle[0] = '\0';
}

void test_peek_widget__cleanup(void) {
}

// Tests
////////////////////////////////////////////////////////////////

void test_peek_widget__publish_passes_through(void) {
  const PeekWidgetInfo info = {
    .icon = 3,
    .title = "Kettle ready",
    .subtitle = "Tap down to open",
    .launch_code = 42,
    .timeout_s = 300,
  };
  cl_assert_equal_i(peek_widget_publish(&info), PEEK_WIDGET_RESULT_SUCCESS);
  cl_assert_equal_i(s_num_publishes, 1);
  cl_assert(uuid_equal(&s_last_publish.owner, &s_app_uuid));
  cl_assert_equal_i(s_last_publish.icon, 3);
  cl_assert_equal_i(s_last_publish.launch_code, 42);
  cl_assert_equal_i(s_last_publish.timeout_s, 300);
  cl_assert_equal_s(s_last_publish_title, "Kettle ready");
  cl_assert_equal_s(s_last_publish_subtitle, "Tap down to open");
}

void test_peek_widget__publish_from_worker_uses_worker_identity(void) {
  s_current_task = PebbleTask_Worker;
  const PeekWidgetInfo info = { .title = "From the worker" };
  cl_assert_equal_i(peek_widget_publish(&info), PEEK_WIDGET_RESULT_SUCCESS);
  cl_assert(uuid_equal(&s_last_publish.owner, &s_worker_uuid));
}

void test_peek_widget__publish_rejects_missing_title(void) {
  cl_assert_equal_i(peek_widget_publish(NULL), PEEK_WIDGET_RESULT_INVALID_ARGS);
  const PeekWidgetInfo no_title = { .subtitle = "sub" };
  cl_assert_equal_i(peek_widget_publish(&no_title), PEEK_WIDGET_RESULT_INVALID_ARGS);
  const PeekWidgetInfo empty_title = { .title = "" };
  cl_assert_equal_i(peek_widget_publish(&empty_title), PEEK_WIDGET_RESULT_INVALID_ARGS);
  cl_assert_equal_i(s_num_publishes, 0);
}

void test_peek_widget__publish_honors_apps_toggle(void) {
  s_apps_enabled = false;
  const PeekWidgetInfo info = { .title = "Kettle ready" };
  cl_assert_equal_i(peek_widget_publish(&info), PEEK_WIDGET_RESULT_DISABLED);
  cl_assert_equal_i(s_num_publishes, 0);
}

void test_peek_widget__publish_truncates_on_codepoint_boundary(void) {
  // 30 ASCII bytes followed by a 3-byte codepoint that straddles the 32-byte limit:
  // the whole codepoint must be dropped, not split.
  char title[64];
  memset(title, 'a', 30);
  strcpy(&title[30], "\xE2\x82\xAC"); // EURO SIGN
  const PeekWidgetInfo info = { .title = title };
  cl_assert_equal_i(peek_widget_publish(&info), PEEK_WIDGET_RESULT_SUCCESS);
  cl_assert_equal_i(strlen(s_last_publish_title), 30);

  // A pure ASCII overflow truncates to exactly the limit.
  char long_title[64];
  memset(long_title, 'b', 40);
  long_title[40] = '\0';
  const PeekWidgetInfo info2 = { .title = long_title };
  cl_assert_equal_i(peek_widget_publish(&info2), PEEK_WIDGET_RESULT_SUCCESS);
  cl_assert_equal_i(strlen(s_last_publish_title), PEEK_WIDGET_TITLE_MAX_LEN);
}

void test_peek_widget__withdraw_uses_caller_identity(void) {
  peek_widget_withdraw();
  cl_assert_equal_i(s_num_withdraws, 1);
  cl_assert(uuid_equal(&s_last_withdraw_owner, &s_app_uuid));

  s_current_task = PebbleTask_Worker;
  peek_widget_withdraw();
  cl_assert_equal_i(s_num_withdraws, 2);
  cl_assert(uuid_equal(&s_last_withdraw_owner, &s_worker_uuid));
}
