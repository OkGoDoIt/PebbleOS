/* SPDX-License-Identifier: Apache-2.0 */

#include "nimble_type_conversions.h"

#include <pbl/bluetooth/audio_companion.h>
#include <pbl/logging/logging.h>
#include <pbl/services/audio_companion_private.h>

#include <comm/ble/gap_le_connection.h>
#include <comm/bt_conn_mgr.h>
#include <comm/bt_lock.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>
#include <system/passert.h>

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

static int prv_access_notify_only(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
  return BLE_ATT_ERR_UNLIKELY;
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
        .access_cb = prv_access_notify_only,
        .flags = BLE_GATT_CHR_F_NOTIFY,
      },
      { 0 },
    },
  },
  { 0 },
};

//! msys blocks the audio companion must leave for everyone else on the shared link.
//!
//! Every outbound packet the host builds -- our notifications, PPoGATT data and ACKs to the
//! official app, ATT responses to its reads, and the watch's own GATT-client traffic to the phone
//! (service discovery after a reconnect, the PPoGATT subscribe, AMS media commands, ANCS requests)
//! -- is an mbuf from the one msys pool. ble_hs_mbuf_from_flat() takes a block per notification,
//! so a drain slice that runs the pool dry starves all of them at once, and two of those consumers
//! do not survive it: an AMS write is dropped ("Couldn't write command"), and a service-discovery
//! step that fails to start never completes, which wedges every later GATT-client procedure until
//! the stack restarts (gatt_client_discovery.c). The 2026-09-14 bug report shows the result on the
//! official app: stuck at "negotiating" for hours, waiting for a PPoGATT ResetRequest the watch
//! could no longer subscribe to send, while this service streamed on untouched.
//!
//! So refuse to allocate below a floor and let the drain's AIMD pacing treat it as backpressure.
//! Data gets the higher floor: a lost control notification is a lost keepalive answer, which the
//! phone counts as the watch being gone and answers with a resync, so control may dip further.
//! Sized against MSYS_1_BLOCK_COUNT (24 with this service, see fw/services/audio_companion/Kconfig):
//! a discovery step, a PPoGATT packet and its ACK, an AMS command and an ATT response each need
//! one, with room for a burst of two.
#define AUDIO_COMPANION_MSYS_FLOOR_DATA (8)
#define AUDIO_COMPANION_MSYS_FLOOR_CONTROL (3)

// The pool must hold the data floor plus the drain's widest slice (DRAIN_MAX_BATCHES_CATCH_UP,
// 12, in audio_companion.c), or catching up meets only refusals. A build that loses the Kconfig
// default of 24 fails here instead of streaming with a third of the pool.
_Static_assert(MYNEWT_VAL(MSYS_1_BLOCK_COUNT) >= AUDIO_COMPANION_MSYS_FLOOR_DATA + 12,
               "msys pool too small for the audio companion's notify floor and catch-up burst");

static bool prv_notify(uint16_t attr_handle, bool subscribed, const uint8_t *data,
                       size_t length, int msys_floor) {
  if (s_conn_handle == AUDIO_COMPANION_INVALID_CONN_HANDLE || !subscribed || !data ||
      length == 0) {
    return false;
  }

  // Unlocked read of the pool's free count: a stale value costs at most one block either way,
  // and the floor has room for that.
  if (os_msys_num_free() <= msys_floor) {
    return false;
  }

  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
  if (!om) {
    return false;
  }

  // ble_gatts_notify_custom() CONSUMES `om` on every path: ble_att_clt_tx_notify() frees it on
  // its own error paths and hands it to ble_att_tx() otherwise, and the caller nulls its copy
  // before its `done:` free. Freeing it here again on failure was a double free into the msys
  // mempool -- one corrupted free-list node per refused notification, which is exactly the
  // backpressure this counter (send_backpressure_events) counts, so it clustered on reconnects
  // and catch-up bursts. The corruption surfaced later as os_memblock_get() walking a cyclic
  // free list inside a critical section (KernelBG "stuck" in the drain callback, the 2026-09-09
  // watchdog loop that stood the feature down) and as a hard fault in os_memblock_get()
  // (2026-09-10 14:28, pc symbolized against the slot-0 image). The mbuf is not ours after the
  // call, whatever it returned.
  const int rc = ble_gatts_notify_custom(s_conn_handle, attr_handle, om);
  if (rc != 0) {
    PBL_LOG_DBG("Audio companion notify failed attr=%u rc=0x%04x", attr_handle, (uint16_t)rc);
    return false;
  }
  return true;
}

static void prv_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify_enabled);
static void prv_handle_disconnect(uint16_t conn_handle);

// NimBLE calls listeners before the connection's own callback (advert.c), for SUBSCRIBE and
// DISCONNECT alike, so the CCCD teardown it reports before a disconnect still arrives first.
static int prv_handle_gap_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_SUBSCRIBE:
      prv_handle_subscribe(event->subscribe.conn_handle, event->subscribe.attr_handle,
                           event->subscribe.cur_notify);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      prv_handle_disconnect(event->disconnect.conn.conn_handle);
      break;
    default:
      break;
  }
  return 0;
}

static struct ble_gap_event_listener s_gap_event_listener;

void pbl_bt_audio_companion_service_init(void) {
  int rc = ble_gatts_count_cfg(s_audio_companion_svcs);
  if (rc != 0) {
    PBL_LOG_ERR("Audio companion GATT count failed: 0x%04x", (uint16_t)rc);
    return;
  }

  rc = ble_gatts_add_svcs(s_audio_companion_svcs);
  if (rc != 0) {
    PBL_LOG_ERR("Audio companion GATT add failed: 0x%04x", (uint16_t)rc);
    return;
  }

  // Called on every pbl_bt_start, but the listener list is only cleared on nimble_port_init.
  rc = ble_gap_event_listener_register(&s_gap_event_listener, prv_handle_gap_event, NULL);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    PBL_LOG_ERR("Audio companion GAP listener failed: 0x%04x", (uint16_t)rc);
  }
}

bool pbl_bt_audio_companion_notify_data(const uint8_t *data, size_t length) {
  return prv_notify(s_data_attr_handle, s_data_subscribed, data, length,
                    AUDIO_COMPANION_MSYS_FLOOR_DATA);
}

bool pbl_bt_audio_companion_notify_control(const uint8_t *data, size_t length) {
  return prv_notify(s_control_attr_handle, s_control_subscribed, data, length,
                    AUDIO_COMPANION_MSYS_FLOOR_CONTROL);
}

uint16_t pbl_bt_audio_companion_get_effective_mtu(void) {
  if (s_conn_handle == AUDIO_COMPANION_INVALID_CONN_HANDLE ||
      (!s_data_subscribed && !s_control_subscribed)) {
    return 0;
  }
  return ble_att_mtu(s_conn_handle);
}

void pbl_bt_audio_companion_set_response_time(enum pbl_bt_response_time_state state,
                                              uint16_t max_period_secs) {
  if (s_conn_handle == AUDIO_COMPANION_INVALID_CONN_HANDLE) {
    return;
  }

  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(s_conn_handle, &desc) != 0) {
    return;
  }

  struct pbl_bt_device_internal device;
  nimble_addr_to_pebble_device(&desc.peer_id_addr, &device);

  bt_lock();
  GAPLEConnection *connection = gap_le_connection_by_device(&device);
  if (connection) {
    conn_mgr_set_ble_conn_response_time(connection, PBL_BT_CONSUMER_AUDIO_COMPANION, state,
                                        max_period_secs);
  }
  bt_unlock();
}

static void prv_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify_enabled) {
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
  }
  // Deliberately NOT invalidated when both subscriptions drop: NimBLE tears down CCCDs
  // (BLE_GAP_EVENT_SUBSCRIBE, reason TERM) before it dispatches BLE_GAP_EVENT_DISCONNECT, so
  // clearing the handle here made the disconnect callback below unreachable on every real link
  // drop and the service-level session state survived disconnects it must not survive.
  audio_companion_handle_subscription_change(s_data_subscribed, s_control_subscribed);
}

static void prv_handle_disconnect(uint16_t conn_handle) {
  if (conn_handle != s_conn_handle) {
    return;
  }
  s_conn_handle = AUDIO_COMPANION_INVALID_CONN_HANDLE;
  s_data_subscribed = false;
  s_control_subscribed = false;
  audio_companion_handle_disconnect();
}
