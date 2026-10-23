/* SPDX-FileCopyrightText: 2025 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "gatt_priv.h"
#include "nimble_type_conversions.h"

#include <pbl/bluetooth/gatt.h>
#include <pbl/logging/logging.h>

#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>
#include <services/gatt/ble_svc_gatt.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

void pbl_bt_gatt_respond_read_subscription(uint32_t transaction_id, uint16_t response_code) {
}

// The Service Changed characteristic lives in the GATT profile service (0x1801),
// registered by ble_svc_gatt_init().
static int prv_find_service_changed_val_handle(uint16_t *val_handle) {
  const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(PBL_BT_GATT_SERVICE_UUID);
  const ble_uuid16_t chr_uuid = BLE_UUID16_INIT(BLE_SVC_GATT_CHR_SERVICE_CHANGED_UUID16);
  return ble_gatts_find_chr(&svc_uuid.u, &chr_uuid.u, nullptr, val_handle);
}

void nimble_gatt_handle_subscribe_event(const struct ble_gap_event *event) {
  if (event->subscribe.prev_indicate == event->subscribe.cur_indicate) {
    return;
  }

  uint16_t val_handle;
  if (prv_find_service_changed_val_handle(&val_handle) != 0 ||
      event->subscribe.attr_handle != val_handle) {
    return;
  }

  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->subscribe.conn_handle, &desc) != 0) {
    return;
  }

  // Also reached on reconnect, when NimBLE restores a bonded peer's CCCD
  // (BLE_GAP_SUBSCRIBE_REASON_RESTORE): a bonded phone does not write it again.
  struct pbl_bt_gatt_server_subscribe_event subscribe_event = {
    .connection_id = event->subscribe.conn_handle,
    .is_subscribing = event->subscribe.cur_indicate,
  };
  nimble_addr_to_pebble_addr(&desc.peer_id_addr, &subscribe_event.dev_address);
  pbl_bt_cb_gatt_service_changed_server_subscribe(&subscribe_event);
}

void pbl_bt_gatt_send_changed_indication(const struct pbl_bt_device_internal *device,
                                         const struct pbl_bt_att_handle_range *data) {
  // Resolve the NimBLE connection handle through the NimBLE connection table
  // (ble_gap_conn_find_by_addr). No bt_lock needed — the device address is a
  // value copy from the service layer, which held bt_lock when it took the copy.
  uint16_t conn_handle;
  if (!pebble_device_to_nimble_conn_handle(device, &conn_handle)) {
    PBL_LOG_ERR("Service Changed: no connection handle for device");
    return;
  }

  uint16_t val_handle;
  int rc = prv_find_service_changed_val_handle(&val_handle);
  if (rc != 0) {
    PBL_LOG_ERR("Service Changed: find_chr failed: 0x%04x", (uint16_t)rc);
    return;
  }

  // The indication value is the changed ATT handle range (start/end, little-endian).
  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, sizeof(*data));
  if (!om) {
    PBL_LOG_ERR("Service Changed: failed to allocate mbuf");
    return;
  }

  rc = ble_gatts_indicate_custom(conn_handle, val_handle, om);
  if (rc != 0) {
    PBL_LOG_ERR("Service Changed: indicate failed: 0x%04x", (uint16_t)rc);
  }
}
