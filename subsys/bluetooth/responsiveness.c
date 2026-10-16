/* SPDX-FileCopyrightText: 2025 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "nimble_type_conversions.h"

#include <pbl/bluetooth/responsiveness.h>
#include <pbl/logging/logging.h>

#include <host/ble_gap.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

bool pbl_bt_le_connection_parameter_update(const struct pbl_bt_device_internal *addr,
                                           const struct pbl_bt_conn_params_update_req *req) {
  uint16_t conn_handle;
  struct ble_gap_upd_params params;
  int rc;

  if (!pebble_device_to_nimble_conn_handle(addr, &conn_handle)) {
    return false;
  }

  pebble_conn_update_to_nimble(req, &params);

  PBL_LOG_DBG(
      "Request connection parameters: "
      "interval=(%u, %u) ms, latency=%u, spvn timeout=%u ms",
      params.itvl_min * BLE_HCI_CONN_ITVL / 1000, params.itvl_max * BLE_HCI_CONN_ITVL / 1000,
      params.latency, params.supervision_timeout * BLE_HCI_CONN_SPVN_TMO_UNITS);

  rc = ble_gap_update_params(conn_handle, &params);
  if (rc != 0) {
    PBL_LOG_ERR("ble_gap_update_params failed: 0x%04x", (uint16_t)rc);
    return false;
  }

  return true;
}
