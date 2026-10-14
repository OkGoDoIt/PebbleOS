/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "gatt_priv.h"

#include <string.h>

#include <pbl/bluetooth/gatt.h>

#include <clar.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <os/os_mbuf.h>

// Stubs
///////////////////////////////////////////////////////////

#include <stubs_logging.h>
#include <stubs_nimble_npl.h>
#include <stubs_passert.h>

// Fakes: the bits of NimBLE the glue calls
///////////////////////////////////////////////////////////

#define SC_VAL_HANDLE    (3)
#define OTHER_VAL_HANDLE (42)
#define CONN_HANDLE      (1)

static bool s_sc_registered;

// One connection, to a peer whose identity and over-the-air addresses differ
// (a bonded phone using a resolvable private address).
static const ble_addr_t s_peer_id_addr = {
  .type = BLE_ADDR_PUBLIC,
  .val = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
};
static const ble_addr_t s_peer_ota_addr = {
  .type = BLE_ADDR_RANDOM,
  .val = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x7f},
};

static int s_indicate_count;
static uint16_t s_indicate_conn_handle;
static uint16_t s_indicate_val_handle;
static uint8_t s_indicate_value[8];
static uint16_t s_indicate_value_len;

int ble_gatts_find_chr(const ble_uuid_t *svc_uuid, const ble_uuid_t *chr_uuid,
                       uint16_t *out_def_handle, uint16_t *out_val_handle) {
  const ble_uuid16_t *svc = (const ble_uuid16_t *)svc_uuid;
  const ble_uuid16_t *chr = (const ble_uuid16_t *)chr_uuid;
  if (!s_sc_registered || svc->u.type != BLE_UUID_TYPE_16 || svc->value != 0x1801 ||
      chr->u.type != BLE_UUID_TYPE_16 || chr->value != 0x2a05) {
    return BLE_HS_ENOENT;
  }
  *out_val_handle = SC_VAL_HANDLE;
  return 0;
}

static void prv_fill_desc(struct ble_gap_conn_desc *desc) {
  memset(desc, 0, sizeof(*desc));
  desc->conn_handle = CONN_HANDLE;
  desc->peer_id_addr = s_peer_id_addr;
  desc->peer_ota_addr = s_peer_ota_addr;
}

int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out_desc) {
  if (handle != CONN_HANDLE) {
    return BLE_HS_ENOTCONN;
  }
  prv_fill_desc(out_desc);
  return 0;
}

int ble_gap_conn_find_by_addr(const ble_addr_t *addr, struct ble_gap_conn_desc *out_desc) {
  if (ble_addr_cmp(addr, &s_peer_id_addr) != 0) {
    return BLE_HS_ENOTCONN;
  }
  prv_fill_desc(out_desc);
  return 0;
}

static struct os_mbuf s_mbuf;
static uint8_t s_mbuf_data[8];

struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len) {
  cl_assert(len <= sizeof(s_mbuf_data));
  memcpy(s_mbuf_data, buf, len);
  s_mbuf = (struct os_mbuf){.om_data = s_mbuf_data, .om_len = len};
  return &s_mbuf;
}

int ble_gatts_indicate_custom(uint16_t conn_handle, uint16_t chr_val_handle, struct os_mbuf *txom) {
  s_indicate_count++;
  s_indicate_conn_handle = conn_handle;
  s_indicate_val_handle = chr_val_handle;
  s_indicate_value_len = txom->om_len;
  memcpy(s_indicate_value, txom->om_data, txom->om_len);
  return 0;
}

// Fakes: the service layer
///////////////////////////////////////////////////////////

static int s_subscribe_count;
static struct pbl_bt_gatt_server_subscribe_event s_last_subscribe;

void pbl_bt_cb_gatt_service_changed_server_subscribe(
    const struct pbl_bt_gatt_server_subscribe_event *event) {
  s_subscribe_count++;
  s_last_subscribe = *event;
}

// Helpers
///////////////////////////////////////////////////////////

static void prv_subscribe_event(uint16_t conn_handle, uint16_t attr_handle, uint8_t reason,
                                bool prev_indicate, bool cur_indicate) {
  struct ble_gap_event event = {
    .type = BLE_GAP_EVENT_SUBSCRIBE,
    .subscribe = {
      .conn_handle = conn_handle,
      .attr_handle = attr_handle,
      .reason = reason,
      .prev_indicate = prev_indicate,
      .cur_indicate = cur_indicate,
    },
  };
  nimble_gatt_handle_subscribe_event(&event);
}

// Tests
///////////////////////////////////////////////////////////

void test_nimble_gatt__initialize(void) {
  s_sc_registered = true;
  s_subscribe_count = 0;
  s_last_subscribe = (struct pbl_bt_gatt_server_subscribe_event){};
  s_indicate_count = 0;
  s_indicate_value_len = 0;
}

void test_nimble_gatt__cccd_write_reports_subscribe(void) {
  prv_subscribe_event(CONN_HANDLE, SC_VAL_HANDLE, BLE_GAP_SUBSCRIBE_REASON_WRITE, false, true);

  cl_assert_equal_i(s_subscribe_count, 1);
  cl_assert(s_last_subscribe.is_subscribing);
  cl_assert_equal_i(s_last_subscribe.connection_id, CONN_HANDLE);
  // The service layer looks the connection up by identity address
  cl_assert_equal_m(s_last_subscribe.dev_address.octets, s_peer_id_addr.val, BLE_DEV_ADDR_LEN);
}

void test_nimble_gatt__restored_cccd_reports_subscribe(void) {
  // A bonded phone does not write the CCCD again on reconnect; NimBLE restores it
  prv_subscribe_event(CONN_HANDLE, SC_VAL_HANDLE, BLE_GAP_SUBSCRIBE_REASON_RESTORE, false, true);

  cl_assert_equal_i(s_subscribe_count, 1);
  cl_assert(s_last_subscribe.is_subscribing);
  cl_assert_equal_m(s_last_subscribe.dev_address.octets, s_peer_id_addr.val, BLE_DEV_ADDR_LEN);
}

void test_nimble_gatt__teardown_reports_unsubscribe(void) {
  prv_subscribe_event(CONN_HANDLE, SC_VAL_HANDLE, BLE_GAP_SUBSCRIBE_REASON_TERM, true, false);

  cl_assert_equal_i(s_subscribe_count, 1);
  cl_assert(!s_last_subscribe.is_subscribing);
}

void test_nimble_gatt__other_characteristic_ignored(void) {
  prv_subscribe_event(CONN_HANDLE, OTHER_VAL_HANDLE, BLE_GAP_SUBSCRIBE_REASON_WRITE, false, true);
  prv_subscribe_event(CONN_HANDLE, OTHER_VAL_HANDLE, BLE_GAP_SUBSCRIBE_REASON_RESTORE, false, true);

  cl_assert_equal_i(s_subscribe_count, 0);
}

void test_nimble_gatt__unchanged_indicate_ignored(void) {
  // Only the notify bit changed
  struct ble_gap_event event = {
    .type = BLE_GAP_EVENT_SUBSCRIBE,
    .subscribe = {
      .conn_handle = CONN_HANDLE,
      .attr_handle = SC_VAL_HANDLE,
      .reason = BLE_GAP_SUBSCRIBE_REASON_WRITE,
      .prev_notify = 0,
      .cur_notify = 1,
      .prev_indicate = 1,
      .cur_indicate = 1,
    },
  };
  nimble_gatt_handle_subscribe_event(&event);

  cl_assert_equal_i(s_subscribe_count, 0);
}

void test_nimble_gatt__unknown_connection_ignored(void) {
  prv_subscribe_event(CONN_HANDLE + 1, SC_VAL_HANDLE, BLE_GAP_SUBSCRIBE_REASON_WRITE, false, true);

  cl_assert_equal_i(s_subscribe_count, 0);
}

void test_nimble_gatt__no_service_changed_characteristic(void) {
  s_sc_registered = false;

  prv_subscribe_event(CONN_HANDLE, SC_VAL_HANDLE, BLE_GAP_SUBSCRIBE_REASON_WRITE, false, true);

  cl_assert_equal_i(s_subscribe_count, 0);
}

void test_nimble_gatt__indication_carries_range_on_service_changed(void) {
  struct pbl_bt_device_internal device = {};
  memcpy(device.address.octets, s_peer_id_addr.val, BLE_DEV_ADDR_LEN);
  const struct pbl_bt_att_handle_range range = {.start = 0x0001, .end = 0xFFFF};

  pbl_bt_gatt_send_changed_indication(&device, &range);

  cl_assert_equal_i(s_indicate_count, 1);
  cl_assert_equal_i(s_indicate_conn_handle, CONN_HANDLE);
  cl_assert_equal_i(s_indicate_val_handle, SC_VAL_HANDLE);
  // Start and end handles, little-endian
  const uint8_t expected[] = {0x01, 0x00, 0xff, 0xff};
  cl_assert_equal_i(s_indicate_value_len, sizeof(expected));
  cl_assert_equal_m(s_indicate_value, expected, sizeof(expected));
}
