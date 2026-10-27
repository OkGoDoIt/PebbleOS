/* SPDX-License-Identifier: Apache-2.0 */
#include <clar.h>
#include <string.h>
#include <stdlib.h>
#include <pbl/services/filesystem/pfs.h>
#include <pbl/services/persist.h>
#include <pbl/services/settings/settings_file.h>
#include <pbl/services/wakeup.h>
#include <process_management/app_manager.h>
#include <services/audio_companion/watch_actions.h>
#include <fake_pbl_malloc.h>
#include <fake_rtc.h>
#include <fake_spi_flash.h>

static SettingsFile s_file;
static bool s_installed;
static bool s_running;
static bool s_wakeup_ok;
static time_t s_deadline;
static Uuid s_uuid;
static PebbleProcessMd s_md;

AppInstallId app_install_get_id_for_uuid(const Uuid *uuid) {
  s_uuid = *uuid;
  return s_installed ? 42 : INSTALL_ID_INVALID;
}
const PebbleProcessMd *app_manager_get_current_app_md(void) {
  s_md.uuid = s_uuid;
  return s_running ? &s_md : NULL;
}
const PebbleProcessMd *app_manager_get_current_worker(void) { return NULL; }
void persist_service_client_open(const Uuid *uuid) {
  cl_must_pass(settings_file_open(&s_file, "native-timers", 4096));
}
void persist_service_client_close(const Uuid *uuid) { settings_file_close(&s_file); }
SettingsFile *persist_service_lock_and_get_store(const Uuid *uuid) { return &s_file; }
void persist_service_unlock_store(SettingsFile *file) {}
status_t wakeup_replace_app_deadline(const Uuid *uuid, time_t timestamp) {
  if (!s_wakeup_ok) { return E_OUT_OF_STORAGE; }
  s_deadline = timestamp;
  return S_SUCCESS;
}

void test_audio_companion_timers__initialize(void) {
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
  fake_rtc_init(0, 1800000000);
  s_installed = true;
  s_running = false;
  s_wakeup_ok = true;
  s_deadline = 0;
}

static bool run(int op, uint32_t id, int seconds, const char *label, char *text) {
  WatchActionRequest request = {.kind = AudioCompanionWatchActionNativeTimers,
    .timers = {.operation = op, .id = id, .seconds = seconds, .label_present = label != NULL}};
  if (label) { strncpy(request.timers.label, label, sizeof(request.timers.label) - 1); }
  text[0] = 0;
  return watch_actions_native_timers(&request, text, 161);
}

static void read_record(uint32_t key, void *value, size_t size) {
  persist_service_client_open(&s_uuid);
  cl_assert_equal_i(settings_file_get_len(&s_file, &key, sizeof(key)), size);
  cl_must_pass(settings_file_get(&s_file, &key, sizeof(key), value, size));
  persist_service_client_close(&s_uuid);
}

void test_audio_companion_timers__native_format_lifecycle_and_reopen(void) {
  char text[161];
  cl_assert(run(1, 0, 90, NULL, text));
  uint32_t id = strtoul(text, NULL, 10);
  cl_assert(id > 0);
  cl_assert_equal_i(s_deadline, 1800000091);
  int32_t count;
  uint8_t record[41];
  read_record(72445846, &count, 4);
  read_record(72445847, record, sizeof(record));
  cl_assert_equal_i(count, 1);
  int64_t start, duration;
  memcpy(&start, record, 8);
  memcpy(&duration, record + 8, 8);
  cl_assert(start == 1800000000000LL && duration == 90000);
  cl_assert_equal_i(record[36], 0); // Same packed paused byte read by the shipping Timer app.
  fake_rtc_init(0, 1800000010);
  cl_assert(run(3, id, 0, NULL, text));
  cl_assert(strstr(text, ",2,80000,90\n"));
  cl_assert_equal_i(s_deadline, 0);
  fake_rtc_init(0, 1800000030);
  cl_assert(run(2, id, 0, NULL, text));
  cl_assert(strstr(text, ",2,80000,90\n"));
  cl_assert(run(5, id, 120, NULL, text));
  cl_assert(strstr(text, ",2,120000,120\n"));
  cl_assert(run(4, id, 0, NULL, text));
  cl_assert_equal_i(s_deadline, 1800000151);
  fake_rtc_init(0, 1800000200);
  cl_assert(run(2, id, 0, NULL, text));
  cl_assert(strstr(text, ",3,0,120\n"));
  cl_assert(run(6, id, 0, NULL, text));
  cl_assert_equal_i(s_deadline, 1800000321);
  cl_assert(run(7, id, 0, NULL, text));
  cl_assert(run(0, 0, 0, NULL, text));
  cl_assert_equal_s(text, "");
  cl_assert(!run(2, id, 0, NULL, text));
}

void test_audio_companion_timers__preserves_native_preferences_and_never_overwrites_full_list(void) {
  char text[161];
  persist_service_client_open(&s_uuid);
  uint32_t sound_key = 76863;
  uint8_t sound = 1;
  cl_must_pass(settings_file_set(&s_file, &sound_key, 4, &sound, 1));
  persist_service_client_close(&s_uuid);
  uint32_t first = 0;
  for (int i = 0; i < 8; ++i) {
    cl_assert(run(1, 0, 60, NULL, text));
    uint32_t id = strtoul(text, NULL, 10);
    if (!i) { first = id; }
    else { cl_assert(id > first); }
  }
  cl_assert(!run(1, 0, 60, NULL, text));
  cl_assert(strstr(text, "full:"));
  cl_assert(run(7, first, 0, NULL, text));
  int32_t count;
  read_record(72445846, &count, 4);
  cl_assert_equal_i(count, 7);
  read_record(sound_key, &sound, 1);
  cl_assert_equal_i(sound, 1);
  cl_assert(run(0, 0, 0, NULL, text));
  cl_assert(strtoul(text, NULL, 10) != first);
}

void test_audio_companion_timers__refuses_unavailable_running_labels_corruption_and_unscheduled_writes(void) {
  char text[161];
  s_installed = false;
  cl_assert(!run(1, 0, 60, NULL, text));
  s_installed = true;
  s_running = true;
  cl_assert(!run(1, 0, 60, NULL, text));
  s_running = false;
  cl_assert(!run(1, 0, 60, "Tea", text));
  s_wakeup_ok = false;
  cl_assert(!run(1, 0, 60, NULL, text));
  s_wakeup_ok = true;
  cl_assert(run(0, 0, 0, NULL, text));
  cl_assert_equal_s(text, "");
  cl_assert(run(1, 0, 60, NULL, text));
  persist_service_client_open(&s_uuid);
  uint32_t key = 72445847;
  uint8_t foreign[37] = {0};
  cl_must_pass(settings_file_set(&s_file, &key, 4, foreign, sizeof(foreign)));
  persist_service_client_close(&s_uuid);
  cl_assert(!run(1, 0, 60, NULL, text));
  cl_assert(strstr(text, "incompatible_data:"));
  read_record(key, foreign, sizeof(foreign));
}
