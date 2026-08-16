/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "services/audio_companion/reboot_trace.h"
#include "system/reboot_reason.h"

#include "stubs_logging.h"
#include "stubs_passert.h"

#include <string.h>

static AudioCompanionRebootTrace s_trace;

void test_audio_companion_reboot_trace__initialize(void) {
  audio_companion_reboot_trace_clear(&s_trace);
}

void test_audio_companion_reboot_trace__cleanup(void) {
}

void test_audio_companion_reboot_trace__cleared_is_empty(void) {
  cl_assert(audio_companion_reboot_trace_is_valid(&s_trace));
  cl_assert_equal_i(s_trace.count, 0);
  cl_assert_equal_i(s_trace.total_reboots, 0);
  cl_assert_equal_i(s_trace.total_error_reboots, 0);
  cl_assert(audio_companion_reboot_trace_newest(&s_trace) == NULL);
}

void test_audio_companion_reboot_trace__error_classification(void) {
  // Benign / intentional restarts are not faults.
  cl_assert(!audio_companion_reboot_trace_is_error_reason(RebootReasonCode_Unknown));
  cl_assert(!audio_companion_reboot_trace_is_error_reason(RebootReasonCode_SoftwareUpdate));
  cl_assert(!audio_companion_reboot_trace_is_error_reason(RebootReasonCode_ShutdownMenuItem));
  cl_assert(!audio_companion_reboot_trace_is_error_reason(RebootReasonCode_ForcedCoreDump));
  // Crash / fault classes are.
  cl_assert(audio_companion_reboot_trace_is_error_reason(RebootReasonCode_Watchdog));
  cl_assert(audio_companion_reboot_trace_is_error_reason(RebootReasonCode_Assert));
  cl_assert(audio_companion_reboot_trace_is_error_reason(RebootReasonCode_StackOverflow));
  cl_assert(audio_companion_reboot_trace_is_error_reason(RebootReasonCode_HardFault));
  cl_assert(audio_companion_reboot_trace_is_error_reason(RebootReasonCode_EventQueueFull));
  cl_assert(audio_companion_reboot_trace_is_error_reason(RebootReasonCode_OutOfMemory));
}

void test_audio_companion_reboot_trace__records_newest(void) {
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_EventQueueFull, true, 1000, NULL);
  const AudioCompanionRebootTraceEntry *newest = audio_companion_reboot_trace_newest(&s_trace);
  cl_assert(newest != NULL);
  cl_assert_equal_i(newest->reason_code, RebootReasonCode_EventQueueFull);
  cl_assert_equal_i(newest->boot_wall_time, 1000);
  cl_assert(newest->flags & AudioCompanionRebootTraceFlagEnabled);
  cl_assert_equal_i(s_trace.count, 1);
  cl_assert_equal_i(s_trace.total_reboots, 1);
  cl_assert_equal_i(s_trace.total_error_reboots, 1);
}

void test_audio_companion_reboot_trace__enabled_flag_reflects_pref(void) {
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, false, 7, NULL);
  const AudioCompanionRebootTraceEntry *newest = audio_companion_reboot_trace_newest(&s_trace);
  cl_assert((newest->flags & AudioCompanionRebootTraceFlagEnabled) == 0);
}

void test_audio_companion_reboot_trace__benign_not_counted_as_fault(void) {
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_SoftwareUpdate, true, 1, NULL);
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 2, NULL);
  cl_assert_equal_i(s_trace.total_reboots, 2);
  cl_assert_equal_i(s_trace.total_error_reboots, 1);
}

void test_audio_companion_reboot_trace__ring_evicts_oldest(void) {
  // Fill past capacity; the newest entries must survive and ordering stay sane.
  const int total = AUDIO_COMPANION_REBOOT_TRACE_ENTRIES + 3;
  for (int i = 0; i < total; i++) {
    audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true,
                                        (uint32_t)(100 + i), NULL);
  }
  cl_assert_equal_i(s_trace.count, AUDIO_COMPANION_REBOOT_TRACE_ENTRIES);
  cl_assert_equal_i(s_trace.total_reboots, total);
  cl_assert_equal_i(s_trace.total_error_reboots, total);
  // Newest entry is the last one recorded.
  const AudioCompanionRebootTraceEntry *newest = audio_companion_reboot_trace_newest(&s_trace);
  cl_assert_equal_i(newest->boot_wall_time, (uint32_t)(100 + total - 1));
  // The oldest surviving entry should be total-ENTRIES, not the very first.
  const uint8_t oldest_idx = s_trace.head;
  cl_assert_equal_i(s_trace.entries[oldest_idx].boot_wall_time,
                    (uint32_t)(100 + total - AUDIO_COMPANION_REBOOT_TRACE_ENTRIES));
}

void test_audio_companion_reboot_trace__counts_saturate(void) {
  s_trace.total_reboots = 0xFFFF;
  s_trace.total_error_reboots = 0xFFFF;
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 1, NULL);
  cl_assert_equal_i(s_trace.total_reboots, 0xFFFF);
  cl_assert_equal_i(s_trace.total_error_reboots, 0xFFFF);
}

void test_audio_companion_reboot_trace__invalid_blob_resets(void) {
  // Simulate a garbage/old-version persisted blob: record() must start fresh
  // rather than trusting bogus indices.
  memset(&s_trace, 0xAB, sizeof(s_trace));
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_HardFault, true, 42, NULL);
  cl_assert(audio_companion_reboot_trace_is_valid(&s_trace));
  cl_assert_equal_i(s_trace.count, 1);
  cl_assert_equal_i(s_trace.total_reboots, 1);
  const AudioCompanionRebootTraceEntry *newest = audio_companion_reboot_trace_newest(&s_trace);
  cl_assert_equal_i(newest->reason_code, RebootReasonCode_HardFault);
}

void test_audio_companion_reboot_trace__reason_names_present(void) {
  cl_assert(strlen(audio_companion_reboot_trace_reason_name(RebootReasonCode_EventQueueFull)) > 0);
  cl_assert(strlen(audio_companion_reboot_trace_reason_name(RebootReasonCode_Watchdog)) > 0);
  cl_assert(strlen(audio_companion_reboot_trace_reason_name(250)) > 0);
}

void test_audio_companion_reboot_trace__records_watchdog_detail(void) {
  // "Watchdog" alone is the fault class, not the culprit. The OS latches which watched tasks
  // failed to check in plus the stuck PC/LR, and the ring has to carry that through.
  const AudioCompanionRebootTraceDetail detail = {
    .watchdog_bits = 0x01,  // KernelMain checked in
    .watchdog_mask = 0x03,  // KernelMain + KernelBackground watched
    .fault_pc = 0x0801a2c4,
    .fault_lr = 0x0801a1f0,
    .fault_extra = 0x0800e3a8,
  };
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 5, &detail);

  const AudioCompanionRebootTraceEntry *newest = audio_companion_reboot_trace_newest(&s_trace);
  cl_assert(newest != NULL);
  cl_assert_equal_i(newest->watchdog_bits, 0x01);
  cl_assert_equal_i(newest->watchdog_mask, 0x03);
  cl_assert_equal_i(newest->fault_pc, 0x0801a2c4);
  cl_assert_equal_i(newest->fault_lr, 0x0801a1f0);
  cl_assert_equal_i(newest->fault_extra, 0x0800e3a8);
  cl_assert_equal_s(audio_companion_reboot_trace_stuck_task_name(newest), "KernelBG");
}

void test_audio_companion_reboot_trace__stuck_task_name_absent_without_detail(void) {
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 5, NULL);
  const AudioCompanionRebootTraceEntry *newest = audio_companion_reboot_trace_newest(&s_trace);
  cl_assert(audio_companion_reboot_trace_stuck_task_name(newest) == NULL);
  cl_assert(audio_companion_reboot_trace_stuck_task_name(NULL) == NULL);

  // Everything checked in: nothing to name, and we must not invent a culprit.
  const AudioCompanionRebootTraceDetail all_fed = { .watchdog_bits = 0x03, .watchdog_mask = 0x03 };
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 6, &all_fed);
  cl_assert(audio_companion_reboot_trace_stuck_task_name(
                audio_companion_reboot_trace_newest(&s_trace)) == NULL);
}

void test_audio_companion_reboot_trace__stuck_task_prefers_lowest_priority(void) {
  // A stalled high-priority task starves the lower-priority ones, so when several are missing the
  // lowest-priority watched task is the one that explains the reset.
  const AudioCompanionRebootTraceDetail detail = {
    .watchdog_bits = 0x00,
    .watchdog_mask = 0x03,  // both KernelMain and KernelBackground missing
  };
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 9, &detail);
  cl_assert_equal_s(
      audio_companion_reboot_trace_stuck_task_name(audio_companion_reboot_trace_newest(&s_trace)),
      "KernelBG");
}

void test_audio_companion_reboot_trace__last_fault_survives_benign_reboots(void) {
  // Recovering from a crash means reloading firmware, which used to push the crash straight out
  // of a six-deep ring. The sticky slot must outlive any number of ordinary restarts.
  const AudioCompanionRebootTraceDetail detail = {
    .watchdog_bits = 0x01,
    .watchdog_mask = 0x03,
    .fault_pc = 0x0801beef,
    .fault_lr = 0x0801cafe,
  };
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 1000, &detail);

  for (int i = 0; i < AUDIO_COMPANION_REBOOT_TRACE_ENTRIES + 2; i++) {
    audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_SoftwareUpdate, true,
                                        (uint32_t)(2000 + i), NULL);
  }

  // Evicted from the ring...
  for (uint8_t i = 0; i < AUDIO_COMPANION_REBOOT_TRACE_ENTRIES; i++) {
    const AudioCompanionRebootTraceEntry *entry = audio_companion_reboot_trace_at(&s_trace, i);
    cl_assert(!entry || entry->reason_code != RebootReasonCode_Watchdog);
  }
  // ...but still reportable.
  const AudioCompanionRebootTraceEntry *fault = audio_companion_reboot_trace_last_fault(&s_trace);
  cl_assert(fault != NULL);
  cl_assert_equal_i(fault->reason_code, RebootReasonCode_Watchdog);
  cl_assert_equal_i(fault->fault_pc, 0x0801beef);
  cl_assert_equal_s(audio_companion_reboot_trace_stuck_task_name(fault), "KernelBG");
}

void test_audio_companion_reboot_trace__no_fault_reported_when_none_recorded(void) {
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_SoftwareUpdate, true, 10, NULL);
  cl_assert(audio_companion_reboot_trace_last_fault(&s_trace) == NULL);
}

void test_audio_companion_reboot_trace__records_session_length_before_fault(void) {
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_SoftwareUpdate, true, 1000, NULL);
  // Crash a day later: the gap between boots is how long that session survived.
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 1000 + 86400,
                                      NULL);
  cl_assert_equal_i(s_trace.last_fault_session_seconds, 86400);

  // A boot loop looks completely different, and must overwrite the previous fault.
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 1000 + 86430,
                                      NULL);
  cl_assert_equal_i(s_trace.last_fault_session_seconds, 30);
}

void test_audio_companion_reboot_trace__session_length_unknown_on_clock_jump(void) {
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_SoftwareUpdate, true, 5000, NULL);
  // RTC went backwards across the reboot: report unknown rather than a bogus duration.
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 100, NULL);
  cl_assert_equal_i(s_trace.last_fault_session_seconds, 0);
}

void test_audio_companion_reboot_trace__at_indexes_from_newest(void) {
  for (int i = 0; i < 3; i++) {
    audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true,
                                        (uint32_t)(100 + i), NULL);
  }
  cl_assert_equal_i(audio_companion_reboot_trace_at(&s_trace, 0)->boot_wall_time, 102);
  cl_assert_equal_i(audio_companion_reboot_trace_at(&s_trace, 1)->boot_wall_time, 101);
  cl_assert_equal_i(audio_companion_reboot_trace_at(&s_trace, 2)->boot_wall_time, 100);
  cl_assert(audio_companion_reboot_trace_at(&s_trace, 3) == NULL);
  cl_assert(audio_companion_reboot_trace_at(NULL, 0) == NULL);
}
