/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/util/attributes.h"

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
//! A session that lasted this long is treated as healthy, clearing the consecutive-fault run.
//! Matches the firmware's own "stable" definition (main.c arms BOOT_BIT_FW_STABLE at 15 minutes),
//! which is the threshold the bootloader's escalation to recovery is keyed off.
#define AUDIO_COMPANION_HEALTHY_SESSION_SECONDS (15 * 60)
//! Consecutive fault boots after which background audio stands itself down.
#define AUDIO_COMPANION_FAULT_LOOP_THRESHOLD (3)
#define AUDIO_COMPANION_REBOOT_TRACE_VERSION (3)
//! v1 recorded only the reason code; v2 added the stuck-task detail the OS already captures;
//! v3 adds the sticky last-fault slot. The entry layout is unchanged between v2 and v3.
#define AUDIO_COMPANION_REBOOT_TRACE_VERSION_V1 (1)
#define AUDIO_COMPANION_REBOOT_TRACE_VERSION_V2 (2)

typedef enum {
  AudioCompanionRebootTraceFlagEnabled = (1 << 0),  //!< background audio pref was on
} AudioCompanionRebootTraceFlags;

typedef struct PACKED {
  uint32_t boot_wall_time;  //!< rtc_get_time() captured when this entry was recorded
  uint8_t reason_code;      //!< RebootReasonCode of the session that just ended
  uint8_t flags;            //!< AudioCompanionRebootTraceFlags bitfield
  //! For a watchdog reset, the task bitsets the OS latched: which tasks had checked in
  //! (@ref watchdog_bits) out of those being watched (@ref watchdog_mask). Tasks in
  //! `mask & ~bits` are the ones that stopped feeding the watchdog. Zero otherwise.
  uint8_t watchdog_bits;
  uint8_t watchdog_mask;
  uint32_t fault_pc;     //!< stuck task PC (watchdog) or fault address; 0 if unknown
  uint32_t fault_lr;     //!< stuck task LR; 0 if unknown
  uint32_t fault_extra;  //!< stuck system-task/timer callback (watchdog); 0 if unknown
} AudioCompanionRebootTraceEntry;

//! Detail captured by the OS for the reboot that just happened, as passed to
//! audio_companion_reboot_trace_record(). All fields zero when nothing was recorded.
typedef struct {
  uint8_t watchdog_bits;
  uint8_t watchdog_mask;
  uint32_t fault_pc;
  uint32_t fault_lr;
  uint32_t fault_extra;
} AudioCompanionRebootTraceDetail;

typedef struct PACKED AudioCompanionRebootTrace {
  uint8_t version;
  uint8_t count;    //!< number of valid entries (<= AUDIO_COMPANION_REBOOT_TRACE_ENTRIES)
  uint8_t head;     //!< ring index of the oldest valid entry
  //! Fault boots since the last session that ran long enough to look healthy. A watch that keeps
  //! crashing before it stabilizes is escalated to recovery firmware by the bootloader, so this
  //! is what lets the feature stand down before it takes the whole watch with it.
  uint8_t consecutive_fault_boots;
  uint16_t total_reboots;        //!< saturating count of every recorded boot
  uint16_t total_error_reboots;  //!< saturating count of crash/fault-class boots
  //! Wall-clock seconds the session before @ref last_fault ran for; 0 when unknown. This is what
  //! separates a one-off after a day of uptime from a boot loop.
  uint32_t last_fault_session_seconds;
  //! Sticky copy of the newest crash/fault-class boot. Ordinary restarts evict entries from the
  //! ring -- reloading firmware after a crash is enough to push the crash out of it -- so keep
  //! the record that actually matters somewhere routine reboots cannot reach.
  AudioCompanionRebootTraceEntry last_fault;
  AudioCompanionRebootTraceEntry entries[AUDIO_COMPANION_REBOOT_TRACE_ENTRIES];
} AudioCompanionRebootTrace;

//! True if the reboot reason denotes a crash/fault rather than a benign,
//! intentional restart (software update, user shutdown, buttons held, ...).
bool audio_companion_reboot_trace_is_error_reason(uint8_t reason_code);

//! Short human-readable name for a RebootReasonCode, for logs and the watch UI.
const char *audio_companion_reboot_trace_reason_name(uint8_t reason_code);

//! Name of the task that stopped feeding the watchdog, derived from the latched bitsets, or NULL
//! when the entry carries no usable watchdog detail. Reported in reverse priority order so the
//! most suspicious task wins, matching the task the OS stored the PC/LR for.
const char *audio_companion_reboot_trace_stuck_task_name(
    const AudioCompanionRebootTraceEntry *entry);

//! Writes *every* task that failed to check in, joined with '+', into @p buf (empty string when
//! the entry carries no watchdog detail). One task alone and several at once mean very different
//! things -- a stalled NewTimers also stops the regular-timer callback that feeds KernelBG while
//! it is idle, so "KernelBG" on its own and "KernelBG+Timers" point at different bugs.
void audio_companion_reboot_trace_stuck_tasks(const AudioCompanionRebootTraceEntry *entry,
                                              char *buf, size_t buf_size);

//! Reset a trace to the empty/initial state.
void audio_companion_reboot_trace_clear(AudioCompanionRebootTrace *trace);

//! True if the trace is structurally sound (matching version, in-range indices).
bool audio_companion_reboot_trace_is_valid(const AudioCompanionRebootTrace *trace);

//! Append a boot record to the ring, evicting the oldest entry when full. @p detail may be NULL
//! when the OS captured no fault detail for this reboot.
void audio_companion_reboot_trace_record(AudioCompanionRebootTrace *trace, uint8_t reason_code,
                                         bool enabled, uint32_t boot_wall_time,
                                         const AudioCompanionRebootTraceDetail *detail);

//! Newest recorded entry, or NULL if the trace is empty.
const AudioCompanionRebootTraceEntry *audio_companion_reboot_trace_newest(
    const AudioCompanionRebootTrace *trace);

//! Ring entry @p index_from_newest boots back (0 == newest), or NULL past the end.
const AudioCompanionRebootTraceEntry *audio_companion_reboot_trace_at(
    const AudioCompanionRebootTrace *trace, uint8_t index_from_newest);

//! Sticky record of the newest fault-class boot, or NULL if none was ever recorded.
const AudioCompanionRebootTraceEntry *audio_companion_reboot_trace_last_fault(
    const AudioCompanionRebootTrace *trace);

//! ---- Persistence (implemented against the settings file on the watch) ----

//! Load the persisted trace into @p trace, leaving it cleared if none/invalid.
void audio_companion_reboot_trace_load(AudioCompanionRebootTrace *trace);

//! Persist @p trace. Best effort; failures are logged, not fatal.
void audio_companion_reboot_trace_save(const AudioCompanionRebootTrace *trace);
