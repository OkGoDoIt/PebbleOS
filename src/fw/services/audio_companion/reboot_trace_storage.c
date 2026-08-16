/* SPDX-License-Identifier: Apache-2.0 */

#include "reboot_trace.h"

#include "pbl/services/settings/settings_file.h"
#include "pbl/logging/logging.h"

#include <inttypes.h>
#include <string.h>

//! Shares the audio companion settings file with the receiver registry (auth.c);
//! the reboot trace lives under its own key. One write per boot, so flash wear is
//! negligible.
#define REBOOT_TRACE_SETTINGS_FILE_NAME "audiocomp"
#define REBOOT_TRACE_SETTINGS_FILE_SIZE (1024)
#define REBOOT_TRACE_KEY "reboottrace"

//! v1 layout, kept only so an upgrade carries the user's restart tallies forward instead of
//! silently resetting them. v1 entries have no stuck-task detail, so those fields stay zero.
typedef struct PACKED {
  uint32_t boot_wall_time;
  uint8_t reason_code;
  uint8_t flags;
  uint16_t reserved;
} RebootTraceEntryV1;

typedef struct PACKED {
  uint8_t version;
  uint8_t count;
  uint8_t head;
  uint8_t reserved;
  uint16_t total_reboots;
  uint16_t total_error_reboots;
  RebootTraceEntryV1 entries[AUDIO_COMPANION_REBOOT_TRACE_ENTRIES];
} RebootTraceV1;

//! v2 differs from v3 only by the sticky last-fault slot, so its entries migrate verbatim.
typedef struct PACKED {
  uint8_t version;
  uint8_t count;
  uint8_t head;
  uint8_t reserved;
  uint16_t total_reboots;
  uint16_t total_error_reboots;
  AudioCompanionRebootTraceEntry entries[AUDIO_COMPANION_REBOOT_TRACE_ENTRIES];
} RebootTraceV2;

static bool prv_indices_sane(uint8_t count, uint8_t head) {
  return count <= AUDIO_COMPANION_REBOOT_TRACE_ENTRIES &&
         head < AUDIO_COMPANION_REBOOT_TRACE_ENTRIES;
}

static void prv_migrate_v1(const RebootTraceV1 *v1, AudioCompanionRebootTrace *trace) {
  if (!prv_indices_sane(v1->count, v1->head)) {
    return;  // leave the cleared ring in place
  }
  trace->count = v1->count;
  trace->head = v1->head;
  trace->total_reboots = v1->total_reboots;
  trace->total_error_reboots = v1->total_error_reboots;
  for (size_t i = 0; i < AUDIO_COMPANION_REBOOT_TRACE_ENTRIES; i++) {
    trace->entries[i].boot_wall_time = v1->entries[i].boot_wall_time;
    trace->entries[i].reason_code = v1->entries[i].reason_code;
    trace->entries[i].flags = v1->entries[i].flags;
  }
}

static void prv_migrate_v2(const RebootTraceV2 *v2, AudioCompanionRebootTrace *trace) {
  if (!prv_indices_sane(v2->count, v2->head)) {
    return;
  }
  trace->count = v2->count;
  trace->head = v2->head;
  trace->total_reboots = v2->total_reboots;
  trace->total_error_reboots = v2->total_error_reboots;
  memcpy(trace->entries, v2->entries, sizeof(trace->entries));
  // v2 had no sticky slot; seed it from the newest fault still in the ring so an upgrade does not
  // start out blind.
  for (uint8_t i = 0; i < trace->count; i++) {
    const AudioCompanionRebootTraceEntry *entry = audio_companion_reboot_trace_at(trace, i);
    if (entry && audio_companion_reboot_trace_is_error_reason(entry->reason_code)) {
      trace->last_fault = *entry;
      break;
    }
  }
}

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
  const int stored_len = settings_file_get_len(&file, REBOOT_TRACE_KEY, strlen(REBOOT_TRACE_KEY));
  if (stored_len == (int)sizeof(AudioCompanionRebootTrace)) {
    AudioCompanionRebootTrace stored;
    const status_t status = settings_file_get(&file, REBOOT_TRACE_KEY, strlen(REBOOT_TRACE_KEY),
                                              &stored, sizeof(stored));
    if (status == S_SUCCESS && audio_companion_reboot_trace_is_valid(&stored)) {
      *trace = stored;
    }
  } else if (stored_len == (int)sizeof(RebootTraceV2)) {
    RebootTraceV2 stored;
    const status_t status = settings_file_get(&file, REBOOT_TRACE_KEY, strlen(REBOOT_TRACE_KEY),
                                              &stored, sizeof(stored));
    if (status == S_SUCCESS && stored.version == AUDIO_COMPANION_REBOOT_TRACE_VERSION_V2) {
      prv_migrate_v2(&stored, trace);
    }
  } else if (stored_len == (int)sizeof(RebootTraceV1)) {
    RebootTraceV1 stored;
    const status_t status = settings_file_get(&file, REBOOT_TRACE_KEY, strlen(REBOOT_TRACE_KEY),
                                              &stored, sizeof(stored));
    if (status == S_SUCCESS && stored.version == AUDIO_COMPANION_REBOOT_TRACE_VERSION_V1) {
      prv_migrate_v1(&stored, trace);
    }
  }
  settings_file_close(&file);
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
