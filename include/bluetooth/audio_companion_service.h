/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <bluetooth/responsiveness.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Boundary between the audio companion OS service and the Bluetooth driver's
//! GATT implementation of the Audio Companion Service (UUID base
//! 7C2B0000-9E4D-4FC2-A2B3-1D6E8A1C9F50; see
//! src/fw/services/audio_companion/PROTOCOL.md).

//! Implemented by the BT driver (src/bluetooth-fw/nimble/audio_companion_service.c;
//! no-op stubs on other backends).
//! @{
//! Register the GATT service. Called during BT driver start.
void bt_driver_audio_companion_service_init(void);
//! Notify one message on the Data characteristic. Returns false when the
//! message cannot be queued (no subscriber, no connection, or mbuf
//! exhaustion); the caller treats that as backpressure.
bool bt_driver_audio_companion_notify_data(const uint8_t *data, size_t length);
//! Notify one message on the Control characteristic. Same return semantics.
bool bt_driver_audio_companion_notify_control(const uint8_t *data, size_t length);
//! Effective ATT MTU of the subscribed connection (0 if none).
uint16_t bt_driver_audio_companion_get_effective_mtu(void);
//! Request a BLE connection interval profile for the subscribed Audio Companion link.
void bt_driver_audio_companion_set_response_time(ResponseTimeState state, uint16_t max_period_secs);
//! @}

//! NimBLE GAP event hooks, called from the backend's central event dispatcher.
//! @{
void bt_driver_audio_companion_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle,
                                                bool notify_enabled);
void bt_driver_audio_companion_handle_disconnect(uint16_t conn_handle);
//! @}

//! Implemented by the OS service (src/fw/services/audio_companion/audio_companion.c).
//! Called from the BT host task; implementations immediately hand off to the
//! system task except audio_companion_fill_info, which is synchronous.
//! @{
void audio_companion_handle_control_write(const uint8_t *data, size_t length);
void audio_companion_handle_subscription_change(bool data_subscribed, bool control_subscribed);
void audio_companion_handle_disconnect(void);
//! Fill the Info characteristic read response. length_in_out carries the
//! buffer capacity in and the written length out.
void audio_companion_fill_info(uint8_t *buf, size_t *length_in_out);
//! @}
