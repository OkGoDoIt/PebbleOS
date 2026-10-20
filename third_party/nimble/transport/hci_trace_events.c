/* SPDX-License-Identifier: Apache-2.0 */

// Events-only HCI trace for the SF32LB52 transport: one line per command and per event, no
// payloads, no ACL. On the bt module so it logs at the normal BT level.

#include <pbl/logging/logging.h>

#include <nimble/hci_common.h>
#include <nimble/transport/hci_h4.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

void prv_hci_trace(uint8_t type, const uint8_t *data, uint16_t len, uint8_t h4tl_packet) {
  if (type == HCI_H4_CMD) {
    const struct ble_hci_cmd *cmd = (const struct ble_hci_cmd *)data;
    PBL_LOG_INFO("HCI CMD 0x%04x len=%u", cmd->opcode, cmd->length);
    return;
  }
  if (type != HCI_H4_EVT) {
    return;
  }
  const struct ble_hci_ev *ev = (const struct ble_hci_ev *)data;
  switch (ev->opcode) {
    case BLE_HCI_EVCODE_NUM_COMP_PKTS:
      break;
    case BLE_HCI_EVCODE_COMMAND_COMPLETE: {
      const struct ble_hci_ev_command_complete *cc = (const void *)ev->data;
      PBL_LOG_INFO("HCI CC 0x%04x st=0x%02x", cc->opcode, cc->status);
      break;
    }
    case BLE_HCI_EVCODE_COMMAND_STATUS: {
      const struct ble_hci_ev_command_status *cs = (const void *)ev->data;
      PBL_LOG_INFO("HCI CS 0x%04x st=0x%02x", cs->opcode, cs->status);
      break;
    }
    case BLE_HCI_EVCODE_DISCONN_CMP: {
      const struct ble_hci_ev_disconn_cmp *dc = (const void *)ev->data;
      PBL_LOG_INFO("HCI DISC h=%u reason=0x%02x", dc->conn_handle, dc->reason);
      break;
    }
    case BLE_HCI_EVCODE_LE_META:
      // Subevent, then (for every connection-related subevent) the status.
      PBL_LOG_INFO("HCI LE 0x%02x st=0x%02x len=%u", ev->data[0], ev->length > 1 ? ev->data[1] : 0xff,
                   ev->length);
      break;
    default:
      PBL_LOG_INFO("HCI EVT 0x%02x len=%u", ev->opcode, ev->length);
      break;
  }
}
