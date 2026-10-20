/* SPDX-License-Identifier: Apache-2.0 */

#include "nimble_type_conversions.h"

#include <string.h>

#include <pbl/bluetooth/muse_setup.h>
#include <pbl/drivers/rtc.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>

#include <comm/ble/gap_le_advert.h>
#include <comm/bt_lock.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>
#include <host/ble_hs_id.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// Meta's Muse Gadget SDK setup service (linux/src/musegadget/ble_server.py).
//! 7fdd3d1c-38ea-46cf-8b46-314ecf5f240c
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x0c, 0x24, 0x5f, 0xcf, 0x4e, 0x31, 0x46, 0x8b,
                     0xcf, 0x46, 0xea, 0x38, 0x1c, 0x3d, 0xdd, 0x7f);
//! 4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01, written by the Muse app.
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x01, 0x9b, 0x8f, 0x5e, 0x2d, 0x3c, 0xf0, 0xa1,
                     0x6e, 0x4a, 0xa2, 0x28, 0x29, 0x30, 0x59, 0x4d);
//! d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c, notified (and readable) to the Muse app.
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0a, 0x8f,
                     0x9c, 0x4e, 0x2b, 0x7b, 0xca, 0xc4, 0x5d, 0xd7);

#define MUSE_SETUP_NAME_PREFIX "MuseGadget"
//! The setup framing's largest packet is 160 bytes (ble_framing.py); leave headroom.
#define MUSE_SETUP_MAX_VALUE (244)
//! Same floor as the audio companion's control notifications: setup traffic is a few hundred
//! bytes in total and must never take the blocks the phone link needs.
#define MUSE_SETUP_MSYS_FLOOR (3)
//! Fast for the first 30 s so the Muse app finds the watch quickly, then calm.
#define MUSE_SETUP_ADV_FAST_MS (30 * 1000)

static uint16_t s_tx_attr_handle;

static struct {
  bool open;
  bool advertising;
  uint16_t conn_handle;
  bool subscribed;
  bool same_phone_known;
  bool same_phone;
  bool resolved;
  uint16_t mtu;
  uint32_t rx_packets;
  uint32_t rx_bytes;
  uint32_t tx_packets;
  uint32_t tx_refused;
  RtcTicks deadline;
  int last_error;
  int last_gap_event;
  uint32_t gap_events;
  char close_reason[24];
  char name[17];
  uint8_t last_tx[MUSE_SETUP_MAX_VALUE];
  uint16_t last_tx_len;
  PblBtMuseSetupRxHandler rx_handler;
} s_state = {
  .conn_handle = BLE_HS_CONN_HANDLE_NONE,
  .last_gap_event = -1,
};

static bool prv_is_muse_link(uint16_t conn_handle) {
  return s_state.conn_handle != BLE_HS_CONN_HANDLE_NONE && conn_handle == s_state.conn_handle;
}

// -----------------------------------------------------------------------------
// GATT

static int prv_access_rx(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  if (!prv_is_muse_link(conn_handle)) {
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  }

  uint8_t data[MUSE_SETUP_MAX_VALUE];
  uint16_t length = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &length) != 0 || length == 0) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }

  bt_lock();
  s_state.rx_packets++;
  s_state.rx_bytes += length;
  PblBtMuseSetupRxHandler handler = s_state.rx_handler;
  bt_unlock();

  if (handler) {
    handler(data, length);
  } else {
    // Loopback for testing the link with a generic BLE app.
    pbl_bt_muse_setup_notify(data, length);
  }
  return 0;
}

static int prv_access_tx(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  if (!prv_is_muse_link(conn_handle)) {
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
  }
  // Meta's gadget answers a TX read with the last notified value.
  bt_lock();
  const int rc = os_mbuf_append(ctxt->om, s_state.last_tx, s_state.last_tx_len);
  bt_unlock();
  return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// No encryption flags: the Muse app talks to a gadget it has never bonded with.
static const struct ble_gatt_svc_def s_muse_setup_svcs[] = {
  {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &s_service_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
      {
        .uuid = &s_rx_uuid.u,
        .access_cb = prv_access_rx,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
      },
      {
        .uuid = &s_tx_uuid.u,
        .access_cb = prv_access_tx,
        .val_handle = &s_tx_attr_handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      },
      { 0 },
    },
  },
  { 0 },
};

bool pbl_bt_muse_setup_notify(const uint8_t *data, size_t length) {
  if (!data || length == 0 || length > MUSE_SETUP_MAX_VALUE) {
    return false;
  }

  bt_lock();
  const uint16_t conn_handle = s_state.conn_handle;
  const bool subscribed = s_state.subscribed;
  memcpy(s_state.last_tx, data, length);
  s_state.last_tx_len = length;
  bt_unlock();

  if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !subscribed ||
      os_msys_num_free() <= MUSE_SETUP_MSYS_FLOOR) {
    bt_lock();
    s_state.tx_refused++;
    bt_unlock();
    return false;
  }

  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
  if (!om) {
    bt_lock();
    s_state.tx_refused++;
    bt_unlock();
    return false;
  }
  // ble_gatts_notify_custom() consumes `om` on every path (see audio_companion_service.c).
  const int rc = ble_gatts_notify_custom(conn_handle, s_tx_attr_handle, om);
  bt_lock();
  if (rc == 0) {
    s_state.tx_packets++;
  } else {
    s_state.tx_refused++;
  }
  bt_unlock();
  return rc == 0;
}

// -----------------------------------------------------------------------------
// Advertising

static int prv_gap_event(struct ble_gap_event *event, void *arg);

static uint32_t prv_ms_left_locked(void) {
  const RtcTicks now = rtc_get_ticks();
  if (now >= s_state.deadline) {
    return 0;
  }
  return (uint32_t)((s_state.deadline - now) * 1000 / RTC_TICKS_HZ);
}

//! bt_lock held. Advertises as the gadget until the deadline or a connection.
static int prv_start_advertising_locked(void) {
  const uint32_t ms_left = prv_ms_left_locked();
  if (ms_left == 0) {
    return BLE_HS_ETIMEOUT;
  }

  // Flags, the setup service UUID and the "paired" byte under the unassigned company id 0xFFFF,
  // as Meta's gadgets advertise them. The name goes in the scan response.
  uint8_t adv[3 + 18 + 5];
  size_t len = 0;
  adv[len++] = 2;
  adv[len++] = BLE_HS_ADV_TYPE_FLAGS;
  adv[len++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv[len++] = 17;
  adv[len++] = BLE_HS_ADV_TYPE_COMP_UUIDS128;
  memcpy(&adv[len], s_service_uuid.value, 16);
  len += 16;
  adv[len++] = 4;
  adv[len++] = BLE_HS_ADV_TYPE_MFG_DATA;
  adv[len++] = 0xFF;
  adv[len++] = 0xFF;
  adv[len++] = 0x00;  // not paired

  uint8_t rsp[2 + sizeof(s_state.name) - 1];
  const size_t name_len = strlen(s_state.name);
  rsp[0] = (uint8_t)(name_len + 1);
  rsp[1] = BLE_HS_ADV_TYPE_COMP_NAME;
  memcpy(&rsp[2], s_state.name, name_len);

  int rc = ble_gap_adv_set_data(adv, len);
  if (rc == 0) {
    rc = ble_gap_adv_rsp_set_data(rsp, 2 + name_len);
  }
  if (rc != 0) {
    return rc;
  }

  const bool fast = ms_left > MUSE_SETUP_ADV_FAST_MS;
  const struct ble_gap_adv_params params = {
    .conn_mode = BLE_GAP_CONN_MODE_UND,
    .disc_mode = BLE_GAP_DISC_MODE_GEN,
    .itvl_min = BLE_GAP_ADV_ITVL_MS(fast ? 30 : 152),
    .itvl_max = BLE_GAP_ADV_ITVL_MS(fast ? 60 : 211),
  };
  rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, (int32_t)ms_left, &params, prv_gap_event, NULL);
  if (rc == 0) {
    s_state.advertising = true;
  }
  return rc;
}

//! bt_lock held.
static void prv_end_window_locked(const char *why) {
  if (!s_state.open) {
    return;
  }
  PBL_LOG_INFO("Muse setup window closed: %s", why);
  s_state.open = false;
  strncpy(s_state.close_reason, why, sizeof(s_state.close_reason) - 1);
  s_state.close_reason[sizeof(s_state.close_reason) - 1] = '\0';
  if (s_state.advertising) {
    ble_gap_adv_stop();
    s_state.advertising = false;
  }
  gap_le_advert_return();
}

bool pbl_bt_muse_setup_open(const char suffix[PBL_BT_MUSE_SETUP_SUFFIX_LEN], uint32_t duration_ms) {
  bool ok = false;
  bt_lock();
  if (s_state.open) {
    ok = true;
    goto unlock;
  }

  // The window's random address replaces whatever random address the host has, so it is only
  // safe while our identity is public (nimble_identity_addr_type()).
  if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, NULL, NULL) != 0) {
    s_state.last_error = BLE_HS_EINVAL;
    PBL_LOG_ERR("Muse setup needs a public identity address");
    goto unlock;
  }

  if (!gap_le_advert_lend()) {
    s_state.last_error = BLE_HS_EBUSY;
    PBL_LOG_ERR("Muse setup: advertiser not available");
    goto unlock;
  }

  ble_addr_t addr;
  int rc = ble_hs_id_gen_rnd(0 /* static, not NRPA */, &addr);
  if (rc == 0) {
    rc = ble_hs_id_set_rnd(addr.val);
  }
  if (rc != 0) {
    s_state.last_error = rc;
    PBL_LOG_ERR("Muse setup: random address failed (0x%04x)", (uint16_t)rc);
    gap_le_advert_return();
    goto unlock;
  }

  strcpy(s_state.name, MUSE_SETUP_NAME_PREFIX);
  memcpy(&s_state.name[sizeof(MUSE_SETUP_NAME_PREFIX) - 1], suffix, PBL_BT_MUSE_SETUP_SUFFIX_LEN);
  s_state.name[sizeof(MUSE_SETUP_NAME_PREFIX) - 1 + PBL_BT_MUSE_SETUP_SUFFIX_LEN] = '\0';
  s_state.deadline = rtc_get_ticks() + (RtcTicks)duration_ms * RTC_TICKS_HZ / 1000;
  s_state.open = true;
  s_state.last_error = 0;
  s_state.last_gap_event = -1;
  s_state.gap_events = 0;
  s_state.close_reason[0] = '\0';
  s_state.same_phone_known = false;
  s_state.rx_packets = s_state.rx_bytes = s_state.tx_packets = s_state.tx_refused = 0;

  rc = prv_start_advertising_locked();
  if (rc != 0) {
    s_state.last_error = rc;
    PBL_LOG_ERR("Muse setup: advertising failed (0x%04x)", (uint16_t)rc);
    prv_end_window_locked("advertising failed");
    goto unlock;
  }
  PBL_LOG_INFO("Muse setup window open as %s for %lu s", s_state.name,
               (unsigned long)(duration_ms / 1000));
  ok = true;

unlock:
  bt_unlock();
  return ok;
}

void pbl_bt_muse_setup_close(void) {
  bt_lock();
  const uint16_t conn_handle = s_state.conn_handle;
  prv_end_window_locked("closed");
  bt_unlock();
  if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
  }
}

void pbl_bt_muse_setup_get_status(PblBtMuseSetupStatus *status) {
  bt_lock();
  *status = (PblBtMuseSetupStatus){
    .open = s_state.open,
    .advertising = s_state.advertising,
    .connected = s_state.conn_handle != BLE_HS_CONN_HANDLE_NONE,
    .same_phone_known = s_state.same_phone_known,
    .same_phone = s_state.same_phone,
    .resolved = s_state.resolved,
    .subscribed = s_state.subscribed,
    .phone_link_up = nimble_conn_phone_link() != BLE_HS_CONN_HANDLE_NONE,
    .mtu = s_state.mtu,
    .rx_packets = s_state.rx_packets,
    .rx_bytes = s_state.rx_bytes,
    .tx_packets = s_state.tx_packets,
    .tx_refused = s_state.tx_refused,
    .seconds_left = s_state.open ? prv_ms_left_locked() / 1000 : 0,
    .last_error = s_state.last_error,
    .last_gap_event = s_state.last_gap_event,
    .gap_events = s_state.gap_events,
  };
  memcpy(status->close_reason, s_state.close_reason, sizeof(status->close_reason));
  memcpy(status->name, s_state.name, sizeof(status->name));
  bt_unlock();
}

void pbl_bt_muse_setup_set_rx_handler(PblBtMuseSetupRxHandler handler) {
  bt_lock();
  s_state.rx_handler = handler;
  bt_unlock();
}

// -----------------------------------------------------------------------------
// The Muse link's own GAP callback. NimBLE delivers every event of a connection to the callback
// that started the advertising it came from, so nothing here reaches advert.c: no GAPLEConnection,
// no ANCS/AMS/PPoGATT, no pairing UI.

static void prv_handle_connect(const struct ble_gap_event *event) {
  if (event->connect.status != 0) {
    // The controller stopped advertising on the CONNECT_IND, so the window must re-advertise.
    PBL_LOG_WRN("Muse setup connect failed (0x%04x)", (uint16_t)event->connect.status);
    s_state.advertising = false;
    s_state.last_error = event->connect.status;
    if (s_state.open) {
      const int rc = prv_start_advertising_locked();
      if (rc != 0) {
        prv_end_window_locked("re-adv after failed connect");
      }
    }
    return;
  }

  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0) {
    return;
  }
  s_state.advertising = false;
  s_state.conn_handle = event->connect.conn_handle;
  s_state.subscribed = false;
  s_state.mtu = ble_att_mtu(event->connect.conn_handle);
  s_state.resolved = ble_addr_cmp(&desc.peer_id_addr, &desc.peer_ota_addr) != 0;
  nimble_conn_set_private_link(event->connect.conn_handle);

  struct ble_gap_conn_desc phone;
  const uint16_t phone_handle = nimble_conn_phone_link();
  s_state.same_phone_known =
      phone_handle != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(phone_handle, &phone) == 0;
  s_state.same_phone =
      s_state.same_phone_known && ble_addr_cmp(&phone.peer_id_addr, &desc.peer_id_addr) == 0;

  PBL_LOG_INFO("Muse setup link up: handle=%u resolved=%u same_phone=%u/%u",
               event->connect.conn_handle, s_state.resolved, s_state.same_phone_known,
               s_state.same_phone);
}

static void prv_handle_disconnect(const struct ble_gap_event *event) {
  if (!prv_is_muse_link(event->disconnect.conn.conn_handle)) {
    return;
  }
  PBL_LOG_INFO("Muse setup link down: reason=0x%x", event->disconnect.reason);
  s_state.conn_handle = BLE_HS_CONN_HANDLE_NONE;
  s_state.subscribed = false;
  nimble_conn_set_private_link(BLE_HS_CONN_HANDLE_NONE);

  // Let the Muse app retry while the window lasts.
  if (s_state.open) {
    const int rc = prv_start_advertising_locked();
    if (rc != 0) {
      s_state.last_error = rc;
      prv_end_window_locked(rc == BLE_HS_ETIMEOUT ? "window ended" : "re-advertising failed");
    }
  }
}

static int prv_gap_event(struct ble_gap_event *event, void *arg) {
  int rc = 0;
  bt_lock();
  s_state.last_gap_event = event->type;
  s_state.gap_events++;
  // Every event on the private link is rare and is the evidence an on-device test needs.
  PBL_LOG_INFO("Muse setup GAP event %d", event->type);
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      prv_handle_connect(event);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      prv_handle_disconnect(event);
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      s_state.advertising = false;
      if (s_state.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        prv_end_window_locked("window ended");
      }
      break;
    case BLE_GAP_EVENT_MTU:
      s_state.mtu = event->mtu.value;
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == s_tx_attr_handle) {
        s_state.subscribed = event->subscribe.cur_notify;
      }
      break;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      *event->conn_update_req.self_params = *event->conn_update_req.peer_params;
      break;
    case BLE_GAP_EVENT_PAIRING_REQUEST:
      // The link is the same iPhone as the bonded phone link: pairing here would go through the
      // phone link's repeat-pairing policy and could delete its bond. The setup never needs it.
      rc = BLE_SM_ERR_PAIR_NOT_SUPP;
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      rc = BLE_GAP_REPEAT_PAIRING_IGNORE;
      break;
    default:
      break;
  }
  bt_unlock();
  return rc;
}

void pbl_bt_muse_setup_service_init(void) {
  // A stack restart drops every link and the advertiser.
  bt_lock();
  s_state.open = false;
  s_state.advertising = false;
  s_state.conn_handle = BLE_HS_CONN_HANDLE_NONE;
  s_state.subscribed = false;
  bt_unlock();
  nimble_conn_set_private_link(BLE_HS_CONN_HANDLE_NONE);

  int rc = ble_gatts_count_cfg(s_muse_setup_svcs);
  if (rc == 0) {
    rc = ble_gatts_add_svcs(s_muse_setup_svcs);
  }
  if (rc != 0) {
    PBL_LOG_ERR("Muse setup GATT add failed: 0x%04x", (uint16_t)rc);
  }
}
