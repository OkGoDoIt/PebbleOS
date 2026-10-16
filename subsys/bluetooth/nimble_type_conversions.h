/* SPDX-FileCopyrightText: 2025 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/bluetooth/responsiveness.h>
#include <pbl/bluetooth/types.h>

#include <host/ble_gap.h>

#define BLE_UUID_SWIZZLE_(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15) \
  a15, a14, a13, a12, a11, a10, a9, a8, a7, a6, a5, a4, a3, a2, a1, a0
#define BLE_UUID_SWIZZLE(x) BLE_UUID_SWIZZLE_(x)

void nimble_addr_to_pebble_addr(const ble_addr_t *addr, struct pbl_bt_addr *addr_out);

void pebble_device_to_nimble_addr(const struct pbl_bt_device_internal *device,
                                  ble_addr_t *addr_out);

void nimble_addr_to_pebble_device(const ble_addr_t *stack_addr,
                                  struct pbl_bt_device_internal *host_addr);

//! The connection handle of a peer's phone link; never a private link (see below).
bool pebble_device_to_nimble_conn_handle(const struct pbl_bt_device_internal *device,
                                         uint16_t *handle);

//! Tracks the phone link the connection layer owns (set on connect, cleared on disconnect).
void nimble_conn_set_phone_link(uint16_t conn_handle);
void nimble_conn_clear_phone_link(uint16_t conn_handle);
uint16_t nimble_conn_phone_link(void);

//! Marks a link PebbleOS must never route phone traffic to; BLE_HS_CONN_HANDLE_NONE clears it.
void nimble_conn_set_private_link(uint16_t conn_handle);
bool nimble_conn_is_private_link(uint16_t conn_handle);

//! Our identity address type: public when the controller has one, whatever random address is set
//! (id.c).
int nimble_identity_addr_type(uint8_t *addr_type);

void nimble_conn_params_to_pebble(struct ble_gap_conn_desc *desc,
                                  struct pbl_bt_conn_params *params);

void pebble_conn_update_to_nimble(const struct pbl_bt_conn_params_update_req *req,
                                  struct ble_gap_upd_params *params);

void nimble_uuid_to_pebble(const ble_uuid_any_t *stack_uuid, Uuid *uuid);
