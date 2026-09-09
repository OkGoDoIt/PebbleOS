/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! What the watch actually hands to DLS, byte for byte.
//!
//! Trap 1's four existing checks (analytics.def parity, the Python schema guard, the compile-time
//! asserts, the ELF layout dump) all describe the record's SHAPE. None of them looks at a record
//! the firmware built, and none of them looks at delivery at all -- so a heartbeat that carries
//! zeros, or one that silently stops being logged, passes every one of them. That gap is why the
//! official app's Battery screen has now been the first thing to notice four separate faults.
//!
//! These tests drive the real backend: set metrics through the published ops, run the heartbeat,
//! and read the bytes back out of a fake dls_log() at the offsets the backend decodes.

#include "clar.h"

#include "pbl/services/analytics/analytics.h"
#include "pbl/services/analytics/backend.h"
#include "pbl/services/analytics/native_heartbeat_stats.h"
#include "pbl/services/data_logging/data_logging_service.h"
#include "pbl/util/build_id.h"

#include "fake_rtc.h"

#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_prompt.h"
#include "stubs_serial.h"

#include <stdint.h>
#include <string.h>

// The offsets the official backend reads for a version-3 record. Deliberately spelled out again
// rather than taken from native.c: a test that derives its expectations from the code under test
// proves only that the code agrees with itself.
#define WIRE_SIZE_V3 563
#define OFF_VERSION 0
#define OFF_BATTERY_SOC_PCT 102
#define OFF_BATTERY_SOC_PCT_SCALE 106
#define OFF_BATTERY_TTE_S 126
#define OFF_WATCHFACE_NAME 330
#define OFF_BATTERY_SOC_PCT_MIN 557

extern void pbl_analytics__native_init(void);
extern void pbl_analytics__native_test_reset(void);
extern void pbl_analytics__native_heartbeat(void);
extern const struct pbl_analytics_backend_ops pbl_analytics__native_ops;

// native.c reads the firmware's build id note out of the image.
const ElfExternalNote TINTIN_BUILD_ID = {
  .name_length = 4,
  .data_length = BUILD_ID_EXPECTED_LEN,
  .type = 3,
  .data = "GNU\0",
};

// ---- fake DLS ----

static DataLoggingSession *s_fake_session;
static uint16_t s_created_item_size;
static bool s_created_buffered;
static uint32_t s_create_count;
static bool s_create_fails;
static DataLoggingResult s_log_result;
static uint8_t s_last_payload[WIRE_SIZE_V3 * 2];
static uint32_t s_last_payload_len;
static uint32_t s_log_count;

DataLoggingSession *dls_create(uint32_t tag, DataLoggingItemType item_type, uint16_t item_size,
                               bool buffered, bool resume, const Uuid *uuid) {
  s_create_count++;
  s_created_item_size = item_size;
  s_created_buffered = buffered;
  if (s_create_fails) {
    return NULL;
  }
  // DataLoggingSession is opaque; native.c only ever passes the handle back to us.
  static uint8_t s_session_storage;
  s_fake_session = (DataLoggingSession *)&s_session_storage;
  return s_fake_session;
}

DataLoggingResult dls_log(DataLoggingSession *session, const void *data, uint32_t num_items) {
  s_log_count++;
  if (s_log_result == DATA_LOGGING_SUCCESS) {
    // The item size the session was created with is what the phone slices the payload by, so that
    // is exactly how many bytes leave the watch per record.
    s_last_payload_len = s_created_item_size * num_items;
    cl_assert(s_last_payload_len <= sizeof(s_last_payload));
    memcpy(s_last_payload, data, s_last_payload_len);
  }
  return s_log_result;
}

// ---- helpers ----

static uint32_t prv_u32_at(size_t offset) {
  uint32_t value;
  memcpy(&value, &s_last_payload[offset], sizeof(value));
  return value;
}

static uint16_t prv_u16_at(size_t offset) {
  uint16_t value;
  memcpy(&value, &s_last_payload[offset], sizeof(value));
  return value;
}

static void prv_set_unsigned(enum pbl_analytics_key key, uint32_t value) {
  pbl_analytics__native_ops.set_unsigned(key, value);
}

void test_analytics_native_heartbeat__initialize(void) {
  s_fake_session = NULL;
  s_created_item_size = 0;
  s_created_buffered = true;
  s_create_count = 0;
  s_create_fails = false;
  s_log_result = DATA_LOGGING_SUCCESS;
  s_last_payload_len = 0;
  s_log_count = 0;
  memset(s_last_payload, 0, sizeof(s_last_payload));
  fake_rtc_init(0, 1000);
  pbl_analytics__native_test_reset();
  pbl_analytics__native_init();
}

void test_analytics_native_heartbeat__cleanup(void) {}

//! The session must be created unbuffered and at the version-3 wire size. A buffered session caps
//! items at 300 bytes and would refuse the record outright.
void test_analytics_native_heartbeat__session_is_unbuffered_at_the_v3_wire_size(void) {
  pbl_analytics__native_heartbeat();

  cl_assert_equal_i(s_create_count, 1);
  cl_assert_equal_i(s_created_item_size, WIRE_SIZE_V3);
  cl_assert(!s_created_buffered);
  cl_assert_equal_i(s_last_payload_len, WIRE_SIZE_V3);
}

//! The whole point of the record: the values the collectors set must land where the backend reads
//! them. A heartbeat full of zeros is what "0% battery, Empty in: --" looks like in the cloud, and
//! nothing else in the tree would notice it.
void test_analytics_native_heartbeat__carries_the_values_the_collectors_set(void) {
  prv_set_unsigned(PBL_ANALYTICS_KEY(battery_soc_pct), 7400);
  prv_set_unsigned(PBL_ANALYTICS_KEY(battery_soc_pct_min), 7100);
  prv_set_unsigned(PBL_ANALYTICS_KEY(battery_tte_s), 86400);
  pbl_analytics__native_ops.set_string(PBL_ANALYTICS_KEY(watchface_name), "MonthGrid");

  pbl_analytics__native_heartbeat();

  cl_assert_equal_i(s_last_payload[OFF_VERSION], 3);
  cl_assert_equal_i(prv_u32_at(OFF_BATTERY_SOC_PCT), 7400);
  cl_assert_equal_i(prv_u16_at(OFF_BATTERY_SOC_PCT_SCALE), 100);
  cl_assert_equal_i(prv_u32_at(OFF_BATTERY_TTE_S), 86400);
  cl_assert_equal_i(prv_u32_at(OFF_BATTERY_SOC_PCT_MIN), 7100);
  cl_assert_equal_s((const char *)&s_last_payload[OFF_WATCHFACE_NAME], "MonthGrid");
}

//! Scaled metrics are only meaningful with their scale, and the backend divides by it. A zero
//! scale would read as 0% however healthy the battery is.
void test_analytics_native_heartbeat__scale_is_never_zero(void) {
  pbl_analytics__native_heartbeat();

  cl_assert(prv_u16_at(OFF_BATTERY_SOC_PCT_SCALE) != 0);
}

//! A failed log must not end heartbeat reporting for the rest of the boot. DATA_LOGGING_CLOSED
//! means the session is gone for good and nothing else recreates it, so the cached handle has to
//! be dropped or every later heartbeat fails against a dead session in silence.
void test_analytics_native_heartbeat__recovers_from_a_failed_log(void) {
  pbl_analytics__native_heartbeat();
  cl_assert_equal_i(s_create_count, 1);

  s_log_result = DATA_LOGGING_CLOSED;
  pbl_analytics__native_heartbeat();
  cl_assert_equal_i(s_create_count, 1);

  s_log_result = DATA_LOGGING_SUCCESS;
  prv_set_unsigned(PBL_ANALYTICS_KEY(battery_soc_pct), 6600);
  pbl_analytics__native_heartbeat();

  cl_assert_equal_i(s_create_count, 2);
  cl_assert_equal_i(prv_u32_at(OFF_BATTERY_SOC_PCT), 6600);
}

//! A session that cannot be created costs one heartbeat, not a reboot loop: the hourly heartbeat
//! would otherwise become an hourly assert.
void test_analytics_native_heartbeat__survives_a_failed_create(void) {
  s_create_fails = true;
  pbl_analytics__native_heartbeat();
  cl_assert_equal_i(s_log_count, 0);

  s_create_fails = false;
  pbl_analytics__native_heartbeat();
  cl_assert_equal_i(s_log_count, 1);
}

//! The counters behind Settings > Audio Companion > Diagnostics. They are the only way to tell,
//! from the watch, whether a Battery-screen fault is upstream or downstream of the record.
void test_analytics_native_heartbeat__accounts_for_what_it_emitted(void) {
  NativeHeartbeatStats stats;

  pbl_analytics_native_get_heartbeat_stats(&stats);
  cl_assert_equal_i(stats.attempts, 0);
  cl_assert_equal_i(stats.last_result, NATIVE_HEARTBEAT_RESULT_NONE);

  prv_set_unsigned(PBL_ANALYTICS_KEY(battery_soc_pct), 7400);
  pbl_analytics__native_heartbeat();

  pbl_analytics_native_get_heartbeat_stats(&stats);
  cl_assert_equal_i(stats.attempts, 1);
  cl_assert_equal_i(stats.logged, 1);
  cl_assert_equal_i(stats.failures, 0);
  cl_assert_equal_i(stats.last_battery_soc_pct, 74);

  s_log_result = DATA_LOGGING_BUSY;
  pbl_analytics__native_heartbeat();

  pbl_analytics_native_get_heartbeat_stats(&stats);
  cl_assert_equal_i(stats.attempts, 2);
  cl_assert_equal_i(stats.logged, 1);
  cl_assert_equal_i(stats.failures, 1);
  cl_assert_equal_i(stats.last_result, DATA_LOGGING_BUSY);
}

//! Storage resets each period, so a metric nobody re-collected reads as zero. That is the shape of
//! a "0% battery" record, and it is worth pinning: it means the value must be collected on every
//! heartbeat, not merely once.
void test_analytics_native_heartbeat__period_storage_resets_between_heartbeats(void) {
  prv_set_unsigned(PBL_ANALYTICS_KEY(battery_soc_pct), 7400);
  pbl_analytics__native_heartbeat();
  cl_assert_equal_i(prv_u32_at(OFF_BATTERY_SOC_PCT), 7400);

  pbl_analytics__native_heartbeat();
  cl_assert_equal_i(prv_u32_at(OFF_BATTERY_SOC_PCT), 0);
}
