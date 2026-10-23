/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// NimBLE's headers redeclare the port's static inline functions without inline, so an unoptimized
// host build emits all of them, along with references to the port and transport below. Stubbing
// these lets a test link NimBLE glue code without the NimBLE host itself.

#include <host/ble_hs.h>
#include <nimble/transport.h>
#include <pbl/kernel/sched.h>
#include <stubs_thread.h>

// The NPL port's ble_npl_os_started() asks the kernel; there is no default stub for it.
bool pbl_kernel_is_started(void) {
  return true;
}

struct ble_npl_event *npl_pebble_eventq_get(struct ble_npl_eventq *evq, ble_npl_time_t tmo) {
  return NULL;
}

void npl_pebble_eventq_put(struct ble_npl_eventq *evq, struct ble_npl_event *ev) {
}

void npl_pebble_eventq_remove(struct ble_npl_eventq *evq, struct ble_npl_event *ev) {
}

ble_npl_error_t npl_pebble_mutex_init(struct ble_npl_mutex *mu) {
  return BLE_NPL_OK;
}

ble_npl_error_t npl_pebble_mutex_pend(struct ble_npl_mutex *mu, ble_npl_time_t timeout) {
  return BLE_NPL_OK;
}

ble_npl_error_t npl_pebble_mutex_release(struct ble_npl_mutex *mu) {
  return BLE_NPL_OK;
}

ble_npl_error_t npl_pebble_sem_init(struct ble_npl_sem *sem, uint16_t tokens) {
  return BLE_NPL_OK;
}

ble_npl_error_t npl_pebble_sem_pend(struct ble_npl_sem *sem, ble_npl_time_t timeout) {
  return BLE_NPL_OK;
}

ble_npl_error_t npl_pebble_sem_release(struct ble_npl_sem *sem) {
  return BLE_NPL_OK;
}

void npl_pebble_callout_init(struct ble_npl_callout *co, struct ble_npl_eventq *evq,
                             ble_npl_event_fn *ev_cb, void *ev_arg) {
}

ble_npl_error_t npl_pebble_callout_reset(struct ble_npl_callout *co, ble_npl_time_t ticks) {
  return BLE_NPL_OK;
}

void npl_pebble_callout_stop(struct ble_npl_callout *co) {
}

bool npl_pebble_callout_is_active(struct ble_npl_callout *co) {
  return false;
}

ble_npl_time_t npl_pebble_callout_get_ticks(struct ble_npl_callout *co) {
  return 0;
}

ble_npl_time_t npl_pebble_callout_remaining_ticks(struct ble_npl_callout *co, ble_npl_time_t now) {
  return 0;
}

ble_npl_error_t npl_pebble_time_ms_to_ticks(uint32_t ms, ble_npl_time_t *out_ticks) {
  *out_ticks = ms;
  return BLE_NPL_OK;
}

ble_npl_error_t npl_pebble_time_ticks_to_ms(ble_npl_time_t ticks, uint32_t *out_ms) {
  *out_ms = ticks;
  return BLE_NPL_OK;
}

int ble_transport_to_ll_cmd_impl(void *buf) {
  return 0;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om) {
  return 0;
}

int ble_transport_to_ll_iso_impl(struct os_mbuf *om) {
  return 0;
}

int ble_transport_to_hs_evt_impl(void *buf) {
  return 0;
}

int ble_transport_to_hs_acl_impl(struct os_mbuf *om) {
  return 0;
}

int ble_transport_to_hs_iso_impl(struct os_mbuf *om) {
  return 0;
}
