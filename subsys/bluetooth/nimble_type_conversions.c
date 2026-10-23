/* SPDX-FileCopyrightText: 2025 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "nimble_type_conversions.h"

#include <string.h>

#include <pbl/btutil/bt_uuid.h>
#include <pbl/kernel/assert.h>
#include <pbl/logging/logging.h>

#include <host/ble_gap.h>
#include <host/ble_hs.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

void nimble_addr_to_pebble_addr(const ble_addr_t *addr, struct pbl_bt_addr *addr_out) {
  memcpy(&addr_out->octets, &addr->val, BLE_DEV_ADDR_LEN);
}

void pebble_device_to_nimble_addr(const struct pbl_bt_device_internal *device,
                                  ble_addr_t *addr_out) {
  addr_out->type = device->is_random_address ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
  memcpy(&addr_out->val, &device->address.octets, BLE_DEV_ADDR_LEN);
}

void nimble_addr_to_pebble_device(const ble_addr_t *stack_addr,
                                  struct pbl_bt_device_internal *host_addr) {
  nimble_addr_to_pebble_addr(stack_addr, &host_addr->address);
  host_addr->is_random_address = stack_addr->type == BLE_ADDR_RANDOM;
  host_addr->is_classic = false;
}

//! The phone's link, as the connection layer knows it (advert.c tracks it). PebbleOS accepts one
//! phone at a time, so one slot is enough.
static uint16_t s_phone_conn_handle = BLE_HS_CONN_HANDLE_NONE;

//! A link PebbleOS itself must never use: the Muse setup window's connection, which the same
//! iPhone opens alongside its phone link.
static uint16_t s_private_conn_handle = BLE_HS_CONN_HANDLE_NONE;

void nimble_conn_set_phone_link(uint16_t conn_handle) {
  s_phone_conn_handle = conn_handle;
}

void nimble_conn_clear_phone_link(uint16_t conn_handle) {
  if (s_phone_conn_handle == conn_handle) {
    s_phone_conn_handle = BLE_HS_CONN_HANDLE_NONE;
  }
}

uint16_t nimble_conn_phone_link(void) {
  return s_phone_conn_handle;
}

void nimble_conn_set_private_link(uint16_t conn_handle) {
  s_private_conn_handle = conn_handle;
}

bool nimble_conn_is_private_link(uint16_t conn_handle) {
  return conn_handle != BLE_HS_CONN_HANDLE_NONE && conn_handle == s_private_conn_handle;
}

bool pebble_device_to_nimble_conn_handle(const struct pbl_bt_device_internal *device,
                                         uint16_t *handle) {
  ble_addr_t addr;
  struct ble_gap_conn_desc desc;

  pebble_device_to_nimble_addr(device, &addr);

  int rc = ble_gap_conn_find_by_addr(&addr, &desc);
  if (rc == 0 && nimble_conn_is_private_link(desc.conn_handle)) {
    // NimBLE returns the newest link to an address. The same iPhone's private link must never
    // capture its phone link's traffic, so fall back to the phone link when it is that peer.
    struct ble_gap_conn_desc phone;
    if (s_phone_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        ble_gap_conn_find(s_phone_conn_handle, &phone) == 0 &&
        ble_addr_cmp(&phone.peer_id_addr, &desc.peer_id_addr) == 0) {
      desc = phone;
    } else {
      rc = BLE_HS_ENOTCONN;
    }
  }
  if (rc != 0) {
    PBL_LOG_ERR("failed to find connection handle for addr" PBL_BT_ADDR_FMT,
                PBL_BT_ADDR_XPLODE(device->address));
  } else {
    *handle = desc.conn_handle;
  }

  return rc == 0;
}

void nimble_conn_params_to_pebble(struct ble_gap_conn_desc *desc,
                                  struct pbl_bt_conn_params *params) {
  params->conn_interval_1_25ms = desc->conn_itvl;
  params->slave_latency_events = desc->conn_latency;
  params->supervision_timeout_10ms = desc->supervision_timeout;
}

void pebble_conn_update_to_nimble(const struct pbl_bt_conn_params_update_req *req,
                                  struct ble_gap_upd_params *params) {
  params->itvl_min = req->interval_min_1_25ms;
  params->itvl_max = req->interval_max_1_25ms;
  params->latency = req->slave_latency_events;
  params->supervision_timeout = req->supervision_timeout_10ms;
  params->min_ce_len = 0U;
  params->max_ce_len = 0U;
}

void nimble_uuid_to_pebble(const ble_uuid_any_t *stack_uuid, Uuid *uuid) {
  switch (stack_uuid->u.type) {
    case BLE_UUID_TYPE_16:
      *uuid = bt_uuid_expand_16bit(stack_uuid->u16.value);
      break;
    case BLE_UUID_TYPE_32:
      *uuid = bt_uuid_expand_32bit(stack_uuid->u32.value);
      break;
    case BLE_UUID_TYPE_128:
      *uuid = UuidMakeFromLEBytes(stack_uuid->u128.value);
      break;
    default:
      PBL_ASSERTN(0);
  }
}
