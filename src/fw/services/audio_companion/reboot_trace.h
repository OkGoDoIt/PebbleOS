/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/attributes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! On-watch flight recorder for diagnosing unexpected reboots while the audio
//! companion feature is in use. The OS already stores the reason for the most
//! recent reboot in a battery-backed register and logs it at boot; this keeps a
//! small persisted ring of those reasons (plus whether background audio was on)
//! so a user who notices the watch restarted can read *why* from
//! Settings -> Audio Companion -> Status without a tethered log capture. It is
//! the breadcrumb that turns "it randomly reboots" into a specific fault class.

#define AUDIO_COMPANION_REBOOT_TRACE_ENTRIES (6)
#define AUDIO_COMPANION_REBOOT_TRACE_VERSION (1)

typedef enum {
  AudioCompanionRebootTraceFlagEnabled = (1 << 0),  //!< background audio pref was on
} AudioCompanionRebootTraceFlags;

typedef struct PACKED {
  uint32_t boot_wall_time;  //!< rtc_get_time() captured when this entry was recorded
  uint8_t reason_code;      //!< RebootReasonCode of the session that just ended
  uint8_t flags;            //!< AudioCompanionRebootTraceFlags bitfield
  uint16_t reserved;
} AudioCompanionRebootTraceEntry;

typedef struct PACKED AudioCompanionRebootTrace {
  uint8_t version;
  uint8_t count;    //!< number of valid entries (<= AUDIO_COMPANION_REBOOT_TRACE_ENTRIES)
  uint8_t head;     //!< ring index of the oldest valid entry
  uint8_t reserved;
  uint16_t total_reboots;        //!< saturating count of every recorded boot
  uint16_t total_error_reboots;  //!< saturating count of crash/fault-class boots
  AudioCompanionRebootTraceEntry entries[AUDIO_COMPANION_REBOOT_TRACE_ENTRIES];
} AudioCompanionRebootTrace;

//! True if the reboot reason denotes a crash/fault rather than a benign,
//! intentional restart (software update, user shutdown, buttons held, ...).
bool audio_companion_reboot_trace_is_error_reason(uint8_t reason_code);

//! Short human-readable name for a RebootReasonCode, for logs and the watch UI.
const char *audio_companion_reboot_trace_reason_name(uint8_t reason_code);

//! Reset a trace to the empty/initial state.
void audio_companion_reboot_trace_clear(AudioCompanionRebootTrace *trace);

//! True if the trace is structurally sound (matching version, in-range indices).
bool audio_companion_reboot_trace_is_valid(const AudioCompanionRebootTrace *trace);

//! Append a boot record to the ring, evicting the oldest entry when full.
void audio_companion_reboot_trace_record(AudioCompanionRebootTrace *trace, uint8_t reason_code,
                                         bool enabled, uint32_t boot_wall_time);

//! Newest recorded entry, or NULL if the trace is empty.
const AudioCompanionRebootTraceEntry *audio_companion_reboot_trace_newest(
    const AudioCompanionRebootTrace *trace);

//! ---- Persistence (implemented against the settings file on the watch) ----

//! Load the persisted trace into @p trace, leaving it cleared if none/invalid.
void audio_companion_reboot_trace_load(AudioCompanionRebootTrace *trace);

//! Persist @p trace. Best effort; failures are logged, not fatal.
void audio_companion_reboot_trace_save(const AudioCompanionRebootTrace *trace);
