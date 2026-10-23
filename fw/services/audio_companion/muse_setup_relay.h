/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/audio_companion_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! The Muse gadget setup relay: the companion app asks the watch to open a pairing window, the
//! watch hosts Meta's Muse setup service for the Muse app (subsys/bluetooth/services/muse_setup/)
//! and relays every byte between the two apps over the audio companion's control channel. The
//! watch never parses the setup messages; the companion app holds every key. Everything here runs
//! on the system task except the Bluetooth-side callbacks, which only queue work.

//! What the watch shows the user about the window.
typedef enum {
  AudioCompanionMuseStatusWaiting,    //!< advertising; the Muse app has not connected yet
  AudioCompanionMuseStatusConnected,  //!< the Muse app is connected and setup is running
  AudioCompanionMuseStatusClosed,     //!< detail: an AudioCompanionMuseCloseReason
} AudioCompanionMuseStatus;

//! Shows "Pair with Muse?" and eventually calls audio_companion_muse_consent_response(). Called on
//! the system task; implementations defer UI work to KernelMain.
typedef void (*AudioCompanionMuseConsentHandler)(void);
//! Status changes, on the system task; same deferral rule.
typedef void (*AudioCompanionMuseStatusHandler)(AudioCompanionMuseStatus status, uint8_t detail);

void audio_companion_muse_init(void);

void audio_companion_muse_set_ui_handlers(AudioCompanionMuseConsentHandler consent,
                                          AudioCompanionMuseStatusHandler status);

//! The user's answer to the consent prompt. Any task.
void audio_companion_muse_consent_response(bool granted);

//! The user left the status screen while the window was open. Any task.
void audio_companion_muse_cancel_from_watch(void);

//! Control requests from the companion app, with the service lock held. Each returns the ACK
//! status to send.
uint8_t audio_companion_muse_handle_open(const AudioCompanionMuseSetupOpenMsg *msg);
uint8_t audio_companion_muse_handle_close(uint8_t app_reason);
uint8_t audio_companion_muse_handle_tx(const uint8_t *data, size_t length);
uint8_t audio_companion_muse_handle_disconnect(uint16_t delay_ms);

//! The companion app is gone (revoked, or its link dropped): the window closes.
void audio_companion_muse_receiver_gone(void);

//! Provided by audio_companion.c: a control notification to the authorized, subscribed receiver.
//! False when there is none, or the transport refused it.
bool audio_companion_relay_notify_control(const uint8_t *data, size_t length);

//! Unit tests only: back to the state before init, with no UI handlers.
void audio_companion_muse_test_reset(void);
