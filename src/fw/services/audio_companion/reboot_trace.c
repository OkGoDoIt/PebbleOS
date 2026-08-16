/* SPDX-License-Identifier: Apache-2.0 */

#include "reboot_trace.h"

#include "kernel/pebble_tasks.h"
#include "system/reboot_reason.h"
#include "pbl/util/size.h"

#include <string.h>

bool audio_companion_reboot_trace_is_error_reason(uint8_t reason_code) {
  // The reboot reason enum groups intentional/benign restarts below
  // RebootReasonCode_Watchdog and crash/fault classes at or above it.
  return reason_code >= RebootReasonCode_Watchdog;
}

const char *audio_companion_reboot_trace_reason_name(uint8_t reason_code) {
  switch (reason_code) {
    case RebootReasonCode_Unknown:
      return "Unknown";
    case RebootReasonCode_LowBattery:
      return "Low Battery";
    case RebootReasonCode_SoftwareUpdate:
      return "FW Update";
    case RebootReasonCode_ResetButtonsHeld:
      return "Buttons Held";
    case RebootReasonCode_ShutdownMenuItem:
      return "Shutdown";
    case RebootReasonCode_FactoryResetReset:
    case RebootReasonCode_FactoryResetShutdown:
      return "Factory Reset";
    case RebootReasonCode_MfgShutdown:
      return "Mfg Shutdown";
    case RebootReasonCode_Serial:
      return "Serial";
    case RebootReasonCode_RemoteReset:
      return "Remote Reset";
    case RebootReasonCode_PrfReset:
    case RebootReasonCode_PrfIdle:
    case RebootReasonCode_PrfResetButtonsHeld:
      return "Recovery";
    case RebootReasonCode_ForcedCoreDump:
      return "Forced Dump";
    case RebootReasonCode_Watchdog:
      return "Watchdog";
    case RebootReasonCode_Assert:
      return "Assert";
    case RebootReasonCode_StackOverflow:
      return "Stack Overflow";
    case RebootReasonCode_HardFault:
      return "Hard Fault";
    case RebootReasonCode_LauncherPanic:
      return "Launcher Panic";
    case RebootReasonCode_ClockFailure:
      return "Clock Failure";
    case RebootReasonCode_AppHardFault:
      return "App Fault";
    case RebootReasonCode_EventQueueFull:
      return "Event Queue Full";
    case RebootReasonCode_WorkerHardFault:
      return "Worker Fault";
    case RebootReasonCode_OutOfMemory:
      return "Out of Memory";
    case RebootReasonCode_BtCoredump:
      return "BT Coredump";
    case RebootReasonCode_CoreDump:
    case RebootReasonCode_CoreDumpEntryFailed:
      return "Coredump";
    default:
      return "Other";
  }
}

//! The tasks task_watchdog.c watches, ordered least-to-most suspicious exactly as it reports them:
//! a stalled high-priority task starves the lower-priority ones, so the lowest-priority watched
//! task is the one whose stall actually explains the reset.
static const struct {
  uint8_t task;
  const char *name;
} s_watched_tasks[] = {
  { PebbleTask_KernelBackground, "KernelBG" },
  { PebbleTask_KernelMain, "KernelMain" },
  { PebbleTask_PULSE, "PULSE" },
  { PebbleTask_NewTimers, "Timers" },
};

void audio_companion_reboot_trace_stuck_tasks(const AudioCompanionRebootTraceEntry *entry,
                                              char *buf, size_t buf_size) {
  if (!buf || buf_size == 0) {
    return;
  }
  buf[0] = '\0';
  if (!entry || entry->watchdog_mask == 0) {
    return;
  }
  const uint8_t stuck = (uint8_t)(entry->watchdog_mask & (uint8_t)~entry->watchdog_bits);
  size_t used = 0;
  for (size_t i = 0; i < ARRAY_LENGTH(s_watched_tasks); i++) {
    if (s_watched_tasks[i].task >= 8 || !(stuck & (uint8_t)(1 << s_watched_tasks[i].task))) {
      continue;
    }
    const char *name = s_watched_tasks[i].name;
    const size_t name_len = strlen(name);
    const size_t sep_len = (used > 0) ? 1 : 0;
    if (used + sep_len + name_len + 1 > buf_size) {
      break;
    }
    if (sep_len) {
      buf[used++] = '+';
    }
    memcpy(&buf[used], name, name_len);
    used += name_len;
    buf[used] = '\0';
  }
}

const char *audio_companion_reboot_trace_stuck_task_name(
    const AudioCompanionRebootTraceEntry *entry) {
  if (!entry || entry->watchdog_mask == 0) {
    return NULL;
  }
  const uint8_t stuck = (uint8_t)(entry->watchdog_mask & (uint8_t)~entry->watchdog_bits);
  if (stuck == 0) {
    return NULL;
  }
  for (size_t i = 0; i < ARRAY_LENGTH(s_watched_tasks); i++) {
    if (s_watched_tasks[i].task < 8 && (stuck & (uint8_t)(1 << s_watched_tasks[i].task))) {
      return s_watched_tasks[i].name;
    }
  }
  return "Other Task";
}

void audio_companion_reboot_trace_clear(AudioCompanionRebootTrace *trace) {
  if (!trace) {
    return;
  }
  memset(trace, 0, sizeof(*trace));
  trace->version = AUDIO_COMPANION_REBOOT_TRACE_VERSION;
}

bool audio_companion_reboot_trace_is_valid(const AudioCompanionRebootTrace *trace) {
  return trace && trace->version == AUDIO_COMPANION_REBOOT_TRACE_VERSION &&
         trace->count <= AUDIO_COMPANION_REBOOT_TRACE_ENTRIES &&
         trace->head < AUDIO_COMPANION_REBOOT_TRACE_ENTRIES;
}

void audio_companion_reboot_trace_record(AudioCompanionRebootTrace *trace, uint8_t reason_code,
                                         bool enabled, uint32_t boot_wall_time,
                                         const AudioCompanionRebootTraceDetail *detail) {
  if (!trace) {
    return;
  }
  // Treat any unparseable / first-ever record as a fresh ring.
  if (!audio_companion_reboot_trace_is_valid(trace)) {
    audio_companion_reboot_trace_clear(trace);
  }

  const AudioCompanionRebootTraceEntry entry = {
    .boot_wall_time = boot_wall_time,
    .reason_code = reason_code,
    .flags = enabled ? (uint8_t)AudioCompanionRebootTraceFlagEnabled : 0,
    .watchdog_bits = detail ? detail->watchdog_bits : 0,
    .watchdog_mask = detail ? detail->watchdog_mask : 0,
    .fault_pc = detail ? detail->fault_pc : 0,
    .fault_lr = detail ? detail->fault_lr : 0,
    .fault_extra = detail ? detail->fault_extra : 0,
  };

  // How long the session that just ended ran for, from the previous boot's wall time. Only
  // meaningful while the RTC is monotonic across the reboot, so treat a backwards clock as
  // unknown rather than reporting nonsense.
  const AudioCompanionRebootTraceEntry *previous = audio_companion_reboot_trace_newest(trace);
  const uint32_t session_seconds =
      (previous && boot_wall_time > previous->boot_wall_time)
          ? (boot_wall_time - previous->boot_wall_time)
          : 0;

  uint8_t insert_index;
  if (trace->count < AUDIO_COMPANION_REBOOT_TRACE_ENTRIES) {
    insert_index = (trace->head + trace->count) % AUDIO_COMPANION_REBOOT_TRACE_ENTRIES;
    trace->count++;
  } else {
    // Ring full: overwrite the oldest entry and advance the head.
    insert_index = trace->head;
    trace->head = (trace->head + 1) % AUDIO_COMPANION_REBOOT_TRACE_ENTRIES;
  }
  trace->entries[insert_index] = entry;

  if (trace->total_reboots < UINT16_MAX) {
    trace->total_reboots++;
  }
  // A session that survived long enough to look healthy ends any crash run, even if it ultimately
  // ended in a fault -- one crash after a day of uptime is not a loop.
  if (session_seconds >= AUDIO_COMPANION_HEALTHY_SESSION_SECONDS) {
    trace->consecutive_fault_boots = 0;
  }

  if (audio_companion_reboot_trace_is_error_reason(reason_code)) {
    if (trace->total_error_reboots < UINT16_MAX) {
      trace->total_error_reboots++;
    }
    if (trace->consecutive_fault_boots < UINT8_MAX) {
      trace->consecutive_fault_boots++;
    }
    trace->last_fault = entry;
    trace->last_fault_session_seconds = session_seconds;
  }
}

const AudioCompanionRebootTraceEntry *audio_companion_reboot_trace_at(
    const AudioCompanionRebootTrace *trace, uint8_t index_from_newest) {
  if (!trace || index_from_newest >= trace->count) {
    return NULL;
  }
  const uint8_t index = (trace->head + trace->count - 1 - index_from_newest) %
                        AUDIO_COMPANION_REBOOT_TRACE_ENTRIES;
  return &trace->entries[index];
}

const AudioCompanionRebootTraceEntry *audio_companion_reboot_trace_newest(
    const AudioCompanionRebootTrace *trace) {
  return audio_companion_reboot_trace_at(trace, 0);
}

const AudioCompanionRebootTraceEntry *audio_companion_reboot_trace_last_fault(
    const AudioCompanionRebootTrace *trace) {
  if (!trace || !audio_companion_reboot_trace_is_error_reason(trace->last_fault.reason_code)) {
    return NULL;
  }
  return &trace->last_fault;
}
