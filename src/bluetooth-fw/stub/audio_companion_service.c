/* SPDX-License-Identifier: Apache-2.0 */

#include <bluetooth/audio_companion_service.h>

void bt_driver_audio_companion_service_init(void) {
}

bool bt_driver_audio_companion_notify_data(const uint8_t *data, size_t length) {
  return false;
}

bool bt_driver_audio_companion_notify_control(const uint8_t *data, size_t length) {
  return false;
}

uint16_t bt_driver_audio_companion_get_effective_mtu(void) {
  return 0;
}

void bt_driver_audio_companion_set_response_time(ResponseTimeState state,
                                                 uint16_t max_period_secs) {
}

void bt_driver_audio_companion_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle,
                                                bool notify_enabled) {
}

void bt_driver_audio_companion_handle_disconnect(uint16_t conn_handle) {
}
