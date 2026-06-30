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
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_EventQueueFull, true, 1000);
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
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, false, 7);
  const AudioCompanionRebootTraceEntry *newest = audio_companion_reboot_trace_newest(&s_trace);
  cl_assert((newest->flags & AudioCompanionRebootTraceFlagEnabled) == 0);
}

void test_audio_companion_reboot_trace__benign_not_counted_as_fault(void) {
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_SoftwareUpdate, true, 1);
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 2);
  cl_assert_equal_i(s_trace.total_reboots, 2);
  cl_assert_equal_i(s_trace.total_error_reboots, 1);
}

void test_audio_companion_reboot_trace__ring_evicts_oldest(void) {
  // Fill past capacity; the newest entries must survive and ordering stay sane.
  const int total = AUDIO_COMPANION_REBOOT_TRACE_ENTRIES + 3;
  for (int i = 0; i < total; i++) {
    audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true,
                                        (uint32_t)(100 + i));
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
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_Watchdog, true, 1);
  cl_assert_equal_i(s_trace.total_reboots, 0xFFFF);
  cl_assert_equal_i(s_trace.total_error_reboots, 0xFFFF);
}

void test_audio_companion_reboot_trace__invalid_blob_resets(void) {
  // Simulate a garbage/old-version persisted blob: record() must start fresh
  // rather than trusting bogus indices.
  memset(&s_trace, 0xAB, sizeof(s_trace));
  audio_companion_reboot_trace_record(&s_trace, RebootReasonCode_HardFault, true, 42);
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
