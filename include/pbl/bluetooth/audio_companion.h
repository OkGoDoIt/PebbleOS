/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/bluetooth/responsiveness.h>

/**
 * @defgroup bluetooth_audio_companion Audio Companion
 * @ingroup bluetooth
 * @brief Watch-hosted GATT service streaming background microphone audio to a third-party app.
 *
 * UUID base 7C2B0000-9E4D-4FC2-A2B3-1D6E8A1C9F50; the wire protocol is in
 * fw/services/audio_companion/PROTOCOL.md. The @c pbl_bt_audio_companion_* functions are
 * implemented by subsys/bluetooth/audio_companion_service.c; the @c audio_companion_handle_*
 * callbacks by the OS service in fw/services/audio_companion/audio_companion.c.
 * @{
 */

/** @brief Register the GATT service. Called on every pbl_bt_start(). */
void pbl_bt_audio_companion_service_init(void);

/**
 * @brief Notify one message on the Data characteristic.
 *
 * @return false when the message cannot be queued (no subscriber, no connection, or not enough
 *         free buffers); the caller treats that as backpressure.
 */
bool pbl_bt_audio_companion_notify_data(const uint8_t *data, size_t length);

/** @brief Notify one message on the Control characteristic. Same return semantics. */
bool pbl_bt_audio_companion_notify_control(const uint8_t *data, size_t length);

/** @brief Effective ATT MTU of the subscribed connection (0 if none). */
uint16_t pbl_bt_audio_companion_get_effective_mtu(void);

/** @brief Request a connection response time for the subscribed Audio Companion link. */
void pbl_bt_audio_companion_set_response_time(enum pbl_bt_response_time_state state,
                                              uint16_t max_period_secs);

/**
 * @name OS service callbacks
 * Called from the NimBLE host task; implementations hand off to the system task, except
 * audio_companion_fill_info(), which is synchronous.
 * @{
 */
void audio_companion_handle_control_write(const uint8_t *data, size_t length);
void audio_companion_handle_subscription_change(bool data_subscribed, bool control_subscribed);
void audio_companion_handle_disconnect(void);
/**
 * @brief Fill the Info characteristic read response.
 *
 * @param buf Output buffer.
 * @param length_in_out Buffer capacity in, written length out.
 */
void audio_companion_fill_info(uint8_t *buf, size_t *length_in_out);
/** @} */

/** @} */
