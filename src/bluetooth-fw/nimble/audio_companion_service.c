/* SPDX-License-Identifier: Apache-2.0 */

#include <bluetooth/audio_companion_service.h>

#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>
#include <pbl/services/audio_companion_private.h>
#include <system/logging.h>
#include <system/passert.h>

#include "comm/ble/gap_le_connection.h"
#include "comm/bt_conn_mgr.h"
#include "comm/bt_lock.h"
#include "nimble_type_conversions.h"

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

#define AUDIO_COMPANION_INVALID_CONN_HANDLE (0xffff)
#define AUDIO_COMPANION_MAX_CONTROL_WRITE_BYTES (128)

//! 7C2B0001-9E4D-4FC2-A2B3-1D6E8A1C9F50
static const ble_uuid128_t s_audio_companion_service_uuid =
    BLE_UUID128_INIT(0x50, 0x9f, 0x1c, 0x8a, 0x6e, 0x1d, 0xb3, 0xa2,
                     0xc2, 0x4f, 0x4d, 0x9e, 0x01, 0x00, 0x2b, 0x7c);

//! 7C2B0002-9E4D-4FC2-A2B3-1D6E8A1C9F50
static const ble_uuid128_t s_audio_companion_info_uuid =
    BLE_UUID128_INIT(0x50, 0x9f, 0x1c, 0x8a, 0x6e, 0x1d, 0xb3, 0xa2,
                     0xc2, 0x4f, 0x4d, 0x9e, 0x02, 0x00, 0x2b, 0x7c);

//! 7C2B0003-9E4D-4FC2-A2B3-1D6E8A1C9F50
static const ble_uuid128_t s_audio_companion_control_uuid =
    BLE_UUID128_INIT(0x50, 0x9f, 0x1c, 0x8a, 0x6e, 0x1d, 0xb3, 0xa2,
                     0xc2, 0x4f, 0x4d, 0x9e, 0x03, 0x00, 0x2b, 0x7c);

//! 7C2B0004-9E4D-4FC2-A2B3-1D6E8A1C9F50
static const ble_uuid128_t s_audio_companion_data_uuid =
    BLE_UUID128_INIT(0x50, 0x9f, 0x1c, 0x8a, 0x6e, 0x1d, 0xb3, 0xa2,
                     0xc2, 0x4f, 0x4d, 0x9e, 0x04, 0x00, 0x2b, 0x7c);

static uint16_t s_control_attr_handle;
static uint16_t s_data_attr_handle;
static uint16_t s_conn_handle = AUDIO_COMPANION_INVALID_CONN_HANDLE;
static bool s_control_subscribed;
static bool s_data_subscribed;

static int prv_access_info(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  uint8_t info[AUDIO_COMPANION_INFO_SIZE];
  size_t length = sizeof(info);
  audio_companion_fill_info(info, &length);
  if (length == 0 || os_mbuf_append(ctxt->om, info, length) != 0) {
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return 0;
}

static int prv_access_control(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  uint8_t data[AUDIO_COMPANION_MAX_CONTROL_WRITE_BYTES];
  uint16_t length = 0;
  const int rc = ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &length);
  if (rc != 0 || length == 0) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }

  s_conn_handle = conn_handle;
  audio_companion_handle_control_write(data, length);
  return 0;
}

static const struct ble_gatt_svc_def s_audio_companion_svcs[] = {
  {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &s_audio_companion_service_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
      {
        .uuid = &s_audio_companion_info_uuid.u,
        .access_cb = prv_access_info,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
      },
      {
        .uuid = &s_audio_companion_control_uuid.u,
        .access_cb = prv_access_control,
        .val_handle = &s_control_attr_handle,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_NOTIFY,
      },
      {
        .uuid = &s_audio_companion_data_uuid.u,
        .val_handle = &s_data_attr_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
      },
      { 0 },
    },
  },
  { 0 },
};

static bool prv_notify(uint16_t attr_handle, bool subscribed, const uint8_t *data,
                       size_t length) {
  if (s_conn_handle == AUDIO_COMPANION_INVALID_CONN_HANDLE || !subscribed || !data ||
      length == 0) {
    return false;
  }

  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
  if (!om) {
    return false;
  }

  const int rc = ble_gatts_notify_custom(s_conn_handle, attr_handle, om);
  if (rc != 0) {
    os_mbuf_free_chain(om);
    PBL_LOG_DBG("Audio companion notify failed attr=%u rc=0x%04x", attr_handle, (uint16_t)rc);
    return false;
  }
  return true;
}

void bt_driver_audio_companion_service_init(void) {
  int rc = ble_gatts_count_cfg(s_audio_companion_svcs);
  PBL_ASSERTN(rc == 0);
  rc = ble_gatts_add_svcs(s_audio_companion_svcs);
  PBL_ASSERTN(rc == 0);
}

bool bt_driver_audio_companion_notify_data(const uint8_t *data, size_t length) {
  return prv_notify(s_data_attr_handle, s_data_subscribed, data, length);
}

bool bt_driver_audio_companion_notify_control(const uint8_t *data, size_t length) {
  return prv_notify(s_control_attr_handle, s_control_subscribed, data, length);
}

uint16_t bt_driver_audio_companion_get_effective_mtu(void) {
  if (s_conn_handle == AUDIO_COMPANION_INVALID_CONN_HANDLE ||
      (!s_data_subscribed && !s_control_subscribed)) {
    return 0;
  }
  return ble_att_mtu(s_conn_handle);
}

void bt_driver_audio_companion_set_response_time(ResponseTimeState state,
                                                 uint16_t max_period_secs) {
  if (s_conn_handle == AUDIO_COMPANION_INVALID_CONN_HANDLE) {
    return;
  }

  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(s_conn_handle, &desc) != 0) {
    return;
  }

  BTDeviceInternal device;
  nimble_addr_to_pebble_device(&desc.peer_id_addr, &device);

  bt_lock();
  GAPLEConnection *connection = gap_le_connection_by_device(&device);
  if (connection) {
    conn_mgr_set_ble_conn_response_time(connection, BtConsumerAudioCompanion, state,
                                      max_period_secs);
  }
  bt_unlock();
}

void bt_driver_audio_companion_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle,
                                                bool notify_enabled) {
  bool changed = false;
  if (attr_handle == s_data_attr_handle) {
    s_data_subscribed = notify_enabled;
    changed = true;
  } else if (attr_handle == s_control_attr_handle) {
    s_control_subscribed = notify_enabled;
    changed = true;
  }

  if (!changed) {
    return;
  }

  if (notify_enabled) {
    s_conn_handle = conn_handle;
  } else if (!s_data_subscribed && !s_control_subscribed) {
    s_conn_handle = AUDIO_COMPANION_INVALID_CONN_HANDLE;
  }
  audio_companion_handle_subscription_change(s_data_subscribed, s_control_subscribed);
}

void bt_driver_audio_companion_handle_disconnect(uint16_t conn_handle) {
  if (conn_handle != s_conn_handle) {
    return;
  }
  s_conn_handle = AUDIO_COMPANION_INVALID_CONN_HANDLE;
  s_data_subscribed = false;
  s_control_subscribed = false;
  audio_companion_handle_disconnect();
}
