/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Fork-local: emission accounting for the native analytics heartbeat.
//!
//! The hourly heartbeat is the only thing that feeds the official Core Devices app's Battery
//! screen, and every part of that path is invisible from the watch: the record is logged to DLS,
//! DLS decides when to send it, and the backend decodes it in the cloud. When the screen goes
//! wrong there has historically been no way to tell whether the watch even emitted a record --
//! which is why the same investigation has been run four times (Sessions 29, 83, 91, and the
//! 2026-09-09 delivery failure).
//!
//! These counters answer the first question in one glance, from Settings > Audio Companion >
//! Diagnostics: did the watch produce heartbeats, and did DLS accept them? If it did, the fault
//! is downstream of the watch and no amount of changing the record layout will help.
//!
//! See UPSTREAM_MERGE_CHECKLIST.md, Trap 1.

#pragma once

#include <stdint.h>

//! Sentinel for "dls_create() returned NULL", which is not a DataLoggingResult.
#define NATIVE_HEARTBEAT_RESULT_NO_SESSION (-1)
//! Sentinel for "no heartbeat has been attempted yet".
#define NATIVE_HEARTBEAT_RESULT_NONE (-2)

typedef struct {
  //! Heartbeat records built (i.e. how many times the hourly timer reached the backend).
  uint32_t attempts;
  //! Records dls_log() accepted. Accepted is not the same as delivered: DLS only sends while the
  //! run level allows it and a phone session exists.
  uint32_t logged;
  //! Records lost, either because the session could not be created or dls_log() refused them.
  uint32_t failures;
  //! Uptime in seconds at the last accepted record, 0 if there has never been one.
  uint32_t last_logged_uptime_s;
  //! Battery percent carried by the most recently built record. This is the number the Battery
  //! screen should show; if it is sane here and wrong in the cloud, the record is not the problem.
  uint8_t last_battery_soc_pct;
  //! Last dls_log() DataLoggingResult, or one of the NATIVE_HEARTBEAT_RESULT_* sentinels.
  int8_t last_result;
} NativeHeartbeatStats;

void pbl_analytics_native_get_heartbeat_stats(NativeHeartbeatStats *stats_out);
