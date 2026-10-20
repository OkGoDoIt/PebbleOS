/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! The Muse gadget setup window: for a few minutes the watch also advertises Meta's Muse gadget
//! setup service (as `MuseGadgetXXXXXX`, from a random static address so the iPhone it is already
//! connected to sees a new device), and the Muse app connects to it on a second, private link that
//! PebbleOS's connection layer never sees. The setup bytes are relayed to the companion app, which
//! runs the pairing protocol and holds every key; the watch never parses them.

//! Length of the gadget suffix: the six uppercase hex digits after "MuseGadget".
#define PBL_BT_MUSE_SETUP_SUFFIX_LEN (6)

typedef struct {
  //! The window is open (advertising, or the Muse app is connected).
  bool open;
  bool advertising;
  bool connected;
  //! The Muse link's peer resolved to the same identity as the phone link (known once connected
  //! while the phone link is up).
  bool same_phone_known;
  bool same_phone;
  //! The Muse link's over-the-air address differs from its identity address.
  bool resolved;
  bool subscribed;
  //! A phone link is up.
  bool phone_link_up;
  uint16_t mtu;
  uint32_t rx_packets;
  uint32_t rx_bytes;
  uint32_t tx_packets;
  uint32_t tx_refused;
  uint32_t seconds_left;
  //! The last NimBLE error from opening or advertising, 0 if none.
  int last_error;
  //! The last NimBLE GAP event type delivered to the window's own callback (-1 if none yet), and
  //! how many it has received since the window opened.
  int last_gap_event;
  uint32_t gap_events;
  //! Why the window last closed ("" while open or never opened).
  char close_reason[24];
  //! The phone's identity key is out of the controller's resolving list (see the service).
  bool peer_hidden;
  char name[17];
} PblBtMuseSetupStatus;

//! Receives each value the Muse app writes to the setup RX characteristic, on the Bluetooth host
//! task. NULL (the default) echoes it back on TX, for testing the link without a phone app.
typedef void (*PblBtMuseSetupRxHandler)(const uint8_t *data, size_t length);

//! Registers the setup GATT service. Call from the stack's service init, after every other
//! service, so no existing attribute handle moves.
void pbl_bt_muse_setup_service_init(void);

//! Opens the window for `duration_ms`, advertising as "MuseGadget" + `suffix`. Returns false (and
//! records last_error) if it could not start; true if it started or was already open.
bool pbl_bt_muse_setup_open(const char suffix[PBL_BT_MUSE_SETUP_SUFFIX_LEN], uint32_t duration_ms);

//! Closes the window: stops advertising, ends the Muse link, returns the advertiser.
void pbl_bt_muse_setup_close(void);

//! The phone link dropped: the window closes (it only exists alongside a phone link), recording
//! that as the reason.
void pbl_bt_muse_setup_phone_link_lost(void);

void pbl_bt_muse_setup_get_status(PblBtMuseSetupStatus *status);

void pbl_bt_muse_setup_set_rx_handler(PblBtMuseSetupRxHandler handler);

//! Notifies one value on the setup TX characteristic. False if not subscribed or out of buffers.
bool pbl_bt_muse_setup_notify(const uint8_t *data, size_t length);
