/* SPDX-License-Identifier: Apache-2.0 */

#include "reboot_trace.h"

#include "pbl/services/settings/settings_file.h"
#include "system/logging.h"

#include <inttypes.h>
#include <string.h>

//! Shares the audio companion settings file with the receiver registry (auth.c);
//! the reboot trace lives under its own key. One write per boot, so flash wear is
//! negligible.
#define REBOOT_TRACE_SETTINGS_FILE_NAME "audiocomp"
#define REBOOT_TRACE_SETTINGS_FILE_SIZE (1024)
#define REBOOT_TRACE_KEY "reboottrace"

void audio_companion_reboot_trace_load(AudioCompanionRebootTrace *trace) {
  if (!trace) {
    return;
  }
  audio_companion_reboot_trace_clear(trace);

  SettingsFile file;
  if (settings_file_open(&file, REBOOT_TRACE_SETTINGS_FILE_NAME,
                         REBOOT_TRACE_SETTINGS_FILE_SIZE) != S_SUCCESS) {
    return;
  }
  AudioCompanionRebootTrace stored;
  const status_t status = settings_file_get(&file, REBOOT_TRACE_KEY, strlen(REBOOT_TRACE_KEY),
                                             &stored, sizeof(stored));
  settings_file_close(&file);
  if (status == S_SUCCESS && audio_companion_reboot_trace_is_valid(&stored)) {
    *trace = stored;
  }
}

void audio_companion_reboot_trace_save(const AudioCompanionRebootTrace *trace) {
  if (!trace) {
    return;
  }
  SettingsFile file;
  if (settings_file_open(&file, REBOOT_TRACE_SETTINGS_FILE_NAME,
                         REBOOT_TRACE_SETTINGS_FILE_SIZE) != S_SUCCESS) {
    return;
  }
  const status_t status = settings_file_set(&file, REBOOT_TRACE_KEY, strlen(REBOOT_TRACE_KEY),
                                             trace, sizeof(*trace));
  settings_file_close(&file);
  if (status != S_SUCCESS) {
    PBL_LOG_ERR("Failed to persist audio companion reboot trace: %" PRId32, (int32_t)status);
  }
}
