/* SPDX-License-Identifier: Apache-2.0 */

#include "services/audio_companion/muse_setup_relay.h"

#include "pbl/bluetooth/audio_companion.h"
#include "pbl/bluetooth/muse_setup.h"
#include "pbl/services/audio_companion_private.h"

#include "clar.h"
#include "fake_mutex.h"
#include "fake_new_timer.h"
#include "fake_system_task.h"

#include "stubs_logging.h"

#include <string.h>

// The relay between the watch's Muse setup service and the companion app. The Bluetooth side and
// the control channel are faked; everything the relay sends is captured as control notifications.

#define MAX_NOTES (64)

typedef struct {
  size_t length;
  uint8_t data[300];
} Note;

static Note s_notes[MAX_NOTES];
static uint32_t s_note_count;
static bool s_notify_control_ok;
static uint16_t s_effective_mtu;

static PblBtMuseSetupRxHandler s_rx_handler;
static PblBtMuseSetupEventHandler s_event_handler;
static PblBtMuseSetupStatus s_status;
static bool s_open_result;
static int s_open_calls;
static char s_open_suffix[PBL_BT_MUSE_SETUP_SUFFIX_LEN + 1];
static uint32_t s_open_duration_ms;
static int s_close_calls;
static bool s_setup_notify_ok;
static int s_setup_notify_calls;
static uint8_t s_setup_notified[AUDIO_COMPANION_MUSE_TX_MAX_BYTES];
static size_t s_setup_notified_length;
static bool s_disconnect_ok;

static int s_consent_asks;
static int s_status_reports;
static AudioCompanionMuseStatus s_last_status;
static uint8_t s_last_status_detail;

// ---- fakes ----

bool audio_companion_relay_notify_control(const uint8_t *data, size_t length) {
  if (!s_notify_control_ok) {
    return false;
  }
  cl_assert(s_note_count < MAX_NOTES);
  cl_assert(length <= sizeof(s_notes[0].data));
  s_notes[s_note_count].length = length;
  memcpy(s_notes[s_note_count].data, data, length);
  s_note_count++;
  return true;
}

uint16_t pbl_bt_audio_companion_get_effective_mtu(void) {
  return s_effective_mtu;
}

void pbl_bt_muse_setup_set_rx_handler(PblBtMuseSetupRxHandler handler) {
  s_rx_handler = handler;
}

void pbl_bt_muse_setup_set_event_handler(PblBtMuseSetupEventHandler handler) {
  s_event_handler = handler;
}

void pbl_bt_muse_setup_get_status(PblBtMuseSetupStatus *status) {
  *status = s_status;
}

bool pbl_bt_muse_setup_open(const char suffix[PBL_BT_MUSE_SETUP_SUFFIX_LEN],
                            uint32_t duration_ms) {
  s_open_calls++;
  memcpy(s_open_suffix, suffix, PBL_BT_MUSE_SETUP_SUFFIX_LEN);
  s_open_duration_ms = duration_ms;
  if (s_open_result) {
    s_status.open = true;
    s_status.advertising = true;
    s_status.seconds_left = duration_ms / 1000;
  }
  return s_open_result;
}

//! Like the real service: closing reports Closed(Closed) through the event handler, synchronously.
void pbl_bt_muse_setup_close(void) {
  s_close_calls++;
  if (!s_status.open) {
    return;
  }
  s_status = (PblBtMuseSetupStatus){0};
  s_event_handler(PblBtMuseSetupEventClosed, PblBtMuseSetupCloseReasonClosed);
}

bool pbl_bt_muse_setup_notify(const uint8_t *data, size_t length) {
  s_setup_notify_calls++;
  if (!s_setup_notify_ok) {
    return false;
  }
  memcpy(s_setup_notified, data, length);
  s_setup_notified_length = length;
  return true;
}

bool pbl_bt_muse_setup_disconnect_client(void) {
  return s_disconnect_ok;
}

static void prv_consent_handler(void) {
  s_consent_asks++;
}

static void prv_status_handler(AudioCompanionMuseStatus status, uint8_t detail) {
  s_status_reports++;
  s_last_status = status;
  s_last_status_detail = detail;
}

// ---- helpers ----

static void prv_run(void) {
  fake_system_task_callbacks_invoke_pending();
}

static AudioCompanionMuseSetupOpenMsg prv_open_msg(const char *suffix, uint16_t secs) {
  AudioCompanionMuseSetupOpenMsg msg = {
    .msg_id = AudioCompanionCtrlMsgIdMuseSetupOpen,
    .request_token = 0x31,
    .window_secs = secs,
  };
  memcpy(msg.suffix, suffix, AUDIO_COMPANION_MUSE_SUFFIX_LEN);
  return msg;
}

//! Asks for a window, says yes on the watch, and clears what that sent.
static void prv_open_window(void) {
  const AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("F1A69C", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
  audio_companion_muse_consent_response(true);
  prv_run();
  cl_assert(s_status.open);
  cl_assert_equal_i(s_note_count, 1);
  s_note_count = 0;
}

static void prv_connect_client(uint16_t mtu) {
  s_status.connected = true;
  s_status.mtu = mtu;
  s_event_handler(PblBtMuseSetupEventClientConnected, mtu);
  s_status.subscribed = true;
  s_event_handler(PblBtMuseSetupEventSubscribed, 1);
  prv_run();
}

static const Note *prv_note(uint32_t index) {
  cl_assert(index < s_note_count);
  return &s_notes[index];
}

static void prv_assert_event(uint32_t index, uint8_t event, uint8_t detail) {
  const Note *note = prv_note(index);
  cl_assert_equal_i(note->length, sizeof(AudioCompanionMuseSetupEventMsg));
  cl_assert_equal_i(note->data[0], AudioCompanionCtrlMsgIdMuseSetupEvent);
  cl_assert_equal_i(note->data[1], event);
  cl_assert_equal_i(note->data[2], detail);
}

static void prv_assert_rx(uint32_t index, uint8_t seq, uint8_t flags, const uint8_t *data,
                          size_t length) {
  const Note *note = prv_note(index);
  cl_assert_equal_i(note->data[0], AudioCompanionCtrlMsgIdMuseSetupRx);
  cl_assert_equal_i(note->data[1], seq);
  cl_assert_equal_i(note->data[2], flags);
  cl_assert_equal_i(note->data[3], length);
  cl_assert_equal_i(note->length, 4 + length);
  cl_assert(memcmp(note->data + 4, data, length) == 0);
}

static void prv_fire_retry(void) {
  cl_assert(stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id));
  stub_new_timer_fire(s_new_timer_start_param_timer_id);
  prv_run();
}

// ---- setup ----

void test_audio_companion_muse__initialize(void) {
  s_note_count = 0;
  s_notify_control_ok = true;
  s_effective_mtu = 256;
  s_status = (PblBtMuseSetupStatus){0};
  s_open_result = true;
  s_open_calls = 0;
  memset(s_open_suffix, 0, sizeof(s_open_suffix));
  s_open_duration_ms = 0;
  s_close_calls = 0;
  s_setup_notify_ok = true;
  s_setup_notify_calls = 0;
  s_setup_notified_length = 0;
  s_disconnect_ok = true;
  s_consent_asks = 0;
  s_status_reports = 0;
  s_last_status = AudioCompanionMuseStatusWaiting;
  s_last_status_detail = 0;

  audio_companion_muse_test_reset();
  audio_companion_muse_init();
  audio_companion_muse_set_ui_handlers(prv_consent_handler, prv_status_handler);
}

void test_audio_companion_muse__cleanup(void) {
  fake_system_task_callbacks_cleanup();
  stub_new_timer_cleanup();
  fake_mutex_reset(true);
}

// ---- opening ----

void test_audio_companion_muse__open_asks_the_user_first(void) {
  const AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("F1A69C", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
  cl_assert_equal_i(s_consent_asks, 1);
  prv_run();
  cl_assert_equal_i(s_open_calls, 0);
  cl_assert_equal_i(s_note_count, 0);

  // One at a time: a second request while the first is pending is refused.
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusBadState);
  cl_assert_equal_i(s_consent_asks, 1);

  audio_companion_muse_consent_response(true);
  prv_run();
  cl_assert_equal_i(s_open_calls, 1);
  cl_assert_equal_s(s_open_suffix, "F1A69C");
  cl_assert_equal_i(s_open_duration_ms, 300 * 1000);
  cl_assert_equal_i(s_note_count, 1);
  prv_assert_event(0, AudioCompanionMuseEventOpened, 0);
  cl_assert_equal_i(s_last_status, AudioCompanionMuseStatusWaiting);

  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusBadState);
}

void test_audio_companion_muse__open_clamps_the_window(void) {
  AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("ABCDEF", 5);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
  audio_companion_muse_consent_response(true);
  prv_run();
  cl_assert_equal_i(s_open_duration_ms, AUDIO_COMPANION_MUSE_WINDOW_MIN_SECS * 1000);

  cl_assert_equal_i(audio_companion_muse_handle_close(AudioCompanionMuseAppCloseReasonCancelled),
                    AudioCompanionAckStatusOk);
  prv_run();
  msg = prv_open_msg("ABCDEF", 5000);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
  audio_companion_muse_consent_response(true);
  prv_run();
  cl_assert_equal_i(s_open_duration_ms, AUDIO_COMPANION_MUSE_WINDOW_MAX_SECS * 1000);
}

void test_audio_companion_muse__open_rejects_a_bad_suffix(void) {
  AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("f1a69c", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusRejected);
  msg = prv_open_msg("F1A69G", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusRejected);
  cl_assert_equal_i(s_consent_asks, 0);
}

void test_audio_companion_muse__open_fails_closed_without_a_prompt(void) {
  audio_companion_muse_set_ui_handlers(NULL, NULL);
  const AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("F1A69C", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusRejected);
}

void test_audio_companion_muse__declining_on_the_watch_closes(void) {
  const AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("F1A69C", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
  audio_companion_muse_consent_response(false);
  prv_run();
  cl_assert_equal_i(s_open_calls, 0);
  cl_assert_equal_i(s_note_count, 1);
  prv_assert_event(0, AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonDeclinedOnWatch);
  cl_assert_equal_i(s_last_status, AudioCompanionMuseStatusClosed);
  cl_assert_equal_i(s_last_status_detail, AudioCompanionMuseCloseReasonDeclinedOnWatch);

  // A late second answer changes nothing; a new request is allowed.
  audio_companion_muse_consent_response(true);
  prv_run();
  cl_assert_equal_i(s_open_calls, 0);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
}

void test_audio_companion_muse__a_window_that_cannot_start_says_so(void) {
  s_open_result = false;
  const AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("F1A69C", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
  audio_companion_muse_consent_response(true);
  prv_run();
  cl_assert_equal_i(s_note_count, 1);
  prv_assert_event(0, AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonCouldNotStart);
  cl_assert_equal_i(s_last_status_detail, AudioCompanionMuseCloseReasonCouldNotStart);
}

void test_audio_companion_muse__cancel_while_asking(void) {
  const AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("F1A69C", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
  cl_assert_equal_i(audio_companion_muse_handle_close(AudioCompanionMuseAppCloseReasonCancelled),
                    AudioCompanionAckStatusOk);
  prv_run();
  prv_assert_event(0, AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonAppCancelled);
  // The prompt's answer arrives after the request went away: ignored.
  audio_companion_muse_consent_response(true);
  prv_run();
  cl_assert_equal_i(s_open_calls, 0);
}

// ---- relaying ----

void test_audio_companion_muse__writes_are_relayed_in_order(void) {
  prv_open_window();
  static const char first[] = "{\"action\":\"get_device_info\"}";
  static const char second[] = "\xFE\x00\x01{}";
  s_rx_handler((const uint8_t *)first, sizeof(first) - 1);
  s_rx_handler((const uint8_t *)second, sizeof(second) - 1);
  prv_run();
  cl_assert_equal_i(s_note_count, 2);
  prv_assert_rx(0, 0, 0, (const uint8_t *)first, sizeof(first) - 1);
  prv_assert_rx(1, 1, 0, (const uint8_t *)second, sizeof(second) - 1);
}

void test_audio_companion_muse__long_values_are_split_for_the_receivers_mtu(void) {
  prv_open_window();
  uint8_t value[40];
  for (size_t i = 0; i < sizeof(value); i++) {
    value[i] = (uint8_t)i;
  }
  s_effective_mtu = 23;  // 16 bytes of value per notification
  s_rx_handler(value, sizeof(value));
  prv_run();
  cl_assert_equal_i(s_note_count, 3);
  prv_assert_rx(0, 0, AUDIO_COMPANION_MUSE_RX_FLAG_MORE, value, 16);
  prv_assert_rx(1, 0, AUDIO_COMPANION_MUSE_RX_FLAG_MORE, value + 16, 16);
  prv_assert_rx(2, 0, 0, value + 32, 8);

  s_note_count = 0;
  s_effective_mtu = 256;
  uint8_t big[250];
  memset(big, 0x5A, sizeof(big));
  s_rx_handler(big, sizeof(big));
  prv_run();
  cl_assert_equal_i(s_note_count, 2);
  prv_assert_rx(0, 1, AUDIO_COMPANION_MUSE_RX_FLAG_MORE, big, AUDIO_COMPANION_MUSE_RX_MAX_BYTES);
  prv_assert_rx(1, 1, 0, big + AUDIO_COMPANION_MUSE_RX_MAX_BYTES, 1);
}

void test_audio_companion_muse__a_refused_notification_is_retried_in_order(void) {
  prv_open_window();
  s_notify_control_ok = false;
  static const uint8_t a[] = {1, 2, 3};
  static const uint8_t b[] = {4, 5};
  s_rx_handler(a, sizeof(a));
  s_rx_handler(b, sizeof(b));
  prv_run();
  cl_assert_equal_i(s_note_count, 0);

  s_notify_control_ok = true;
  prv_fire_retry();
  cl_assert_equal_i(s_note_count, 2);
  prv_assert_rx(0, 0, 0, a, sizeof(a));
  prv_assert_rx(1, 1, 0, b, sizeof(b));
  cl_assert(!stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id));
}

void test_audio_companion_muse__a_receiver_that_never_listens_does_not_retry_forever(void) {
  prv_open_window();
  s_notify_control_ok = false;
  static const uint8_t a[] = {1, 2, 3};
  s_rx_handler(a, sizeof(a));
  prv_run();
  int fired = 0;
  while (stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id)) {
    stub_new_timer_fire(s_new_timer_start_param_timer_id);
    prv_run();
    cl_assert(++fired < 1000);
  }
  cl_assert(fired >= 50);

  // The queue went with it: a listening receiver later gets only new values.
  s_notify_control_ok = true;
  static const uint8_t b[] = {9};
  s_rx_handler(b, sizeof(b));
  prv_run();
  cl_assert_equal_i(s_note_count, 1);
  prv_assert_rx(0, 1, 0, b, sizeof(b));
}

void test_audio_companion_muse__dropped_writes_are_reported_and_leave_a_gap(void) {
  prv_open_window();
  s_notify_control_ok = false;
  uint8_t value[200];
  memset(value, 0xAB, sizeof(value));
  // 206 bytes per queued value: four fit in the 1 KiB queue, the next two are dropped.
  for (int i = 0; i < 6; i++) {
    s_rx_handler(value, sizeof(value));
  }
  prv_run();
  s_notify_control_ok = true;
  prv_fire_retry();
  cl_assert_equal_i(s_note_count, 5);
  prv_assert_event(0, AudioCompanionMuseEventRxOverflow, 2);
  for (uint32_t i = 0; i < 4; i++) {
    prv_assert_rx(1 + i, (uint8_t)i, 0, value, sizeof(value));
  }
  s_note_count = 0;
  s_rx_handler(value, 1);
  prv_run();
  prv_assert_rx(0, 6, 0, value, 1);  // seq 4 and 5 were the dropped ones
}

void test_audio_companion_muse__writes_outside_a_window_are_ignored(void) {
  static const uint8_t a[] = {1};
  s_rx_handler(a, sizeof(a));
  prv_run();
  cl_assert_equal_i(s_note_count, 0);
  cl_assert(!stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id));
}

void test_audio_companion_muse__tx_needs_a_subscribed_client(void) {
  static const uint8_t reply[] = "{\"type\":\"device_info\"}";
  cl_assert_equal_i(audio_companion_muse_handle_tx(reply, sizeof(reply) - 1),
                    AudioCompanionAckStatusBadState);
  prv_open_window();
  cl_assert_equal_i(audio_companion_muse_handle_tx(reply, sizeof(reply) - 1),
                    AudioCompanionAckStatusRejected);
  cl_assert_equal_i(s_setup_notify_calls, 0);

  prv_connect_client(256);
  cl_assert_equal_i(audio_companion_muse_handle_tx(reply, sizeof(reply) - 1),
                    AudioCompanionAckStatusOk);
  cl_assert_equal_i(s_setup_notified_length, sizeof(reply) - 1);
  cl_assert(memcmp(s_setup_notified, reply, sizeof(reply) - 1) == 0);

  s_setup_notify_ok = false;
  cl_assert_equal_i(audio_companion_muse_handle_tx(reply, sizeof(reply) - 1),
                    AudioCompanionAckStatusBusy);
}

void test_audio_companion_muse__disconnect_needs_a_client(void) {
  s_disconnect_ok = false;
  cl_assert_equal_i(audio_companion_muse_handle_disconnect(250), AudioCompanionAckStatusBadState);
  s_disconnect_ok = true;
  cl_assert_equal_i(audio_companion_muse_handle_disconnect(250), AudioCompanionAckStatusOk);
}

// ---- what the watch shows, and how a window ends ----

void test_audio_companion_muse__client_link_events_reach_both_sides(void) {
  prv_open_window();
  s_status_reports = 0;
  prv_connect_client(256);
  cl_assert_equal_i(s_last_status, AudioCompanionMuseStatusConnected);
  cl_assert_equal_i(s_note_count, 2);
  prv_assert_event(0, AudioCompanionMuseEventClientConnected, 0);
  cl_assert_equal_i(s_notes[0].data[3] | (s_notes[0].data[4] << 8), 256);  // mtu
  prv_assert_event(1, AudioCompanionMuseEventSubscribed, 1);

  s_status.connected = false;
  s_event_handler(PblBtMuseSetupEventClientDisconnected, 0x13);
  prv_run();
  prv_assert_event(2, AudioCompanionMuseEventClientDisconnected, 0x13);
  cl_assert_equal_i(s_last_status, AudioCompanionMuseStatusWaiting);
}

void test_audio_companion_muse__the_app_closing_paired_reads_as_paired(void) {
  prv_open_window();
  prv_connect_client(256);
  s_note_count = 0;
  cl_assert_equal_i(audio_companion_muse_handle_close(AudioCompanionMuseAppCloseReasonPaired),
                    AudioCompanionAckStatusOk);
  prv_run();
  cl_assert_equal_i(s_close_calls, 1);
  cl_assert_equal_i(s_note_count, 1);
  prv_assert_event(0, AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonAppPaired);
  cl_assert_equal_i(s_last_status, AudioCompanionMuseStatusClosed);
  cl_assert_equal_i(s_last_status_detail, AudioCompanionMuseCloseReasonAppPaired);

  static const uint8_t reply[] = {1};
  cl_assert_equal_i(audio_companion_muse_handle_tx(reply, 1), AudioCompanionAckStatusBadState);
  cl_assert_equal_i(audio_companion_muse_handle_close(AudioCompanionMuseAppCloseReasonPaired),
                    AudioCompanionAckStatusBadState);
}

void test_audio_companion_muse__a_timeout_names_whether_muse_ever_connected(void) {
  prv_open_window();
  s_status = (PblBtMuseSetupStatus){0};
  s_event_handler(PblBtMuseSetupEventClosed, PblBtMuseSetupCloseReasonTimeout);
  prv_run();
  prv_assert_event(0, AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonTimeout);
  cl_assert_equal_i(s_last_status, AudioCompanionMuseStatusClosed);
  cl_assert_equal_i(s_last_status_detail, AudioCompanionMuseCloseReasonTimeout);

  // Connected, then timed out: the app hears "timeout", the watch says the pairing didn't finish.
  s_note_count = 0;
  prv_open_window();
  prv_connect_client(256);
  s_note_count = 0;
  s_status = (PblBtMuseSetupStatus){0};
  s_event_handler(PblBtMuseSetupEventClosed, PblBtMuseSetupCloseReasonTimeout);
  prv_run();
  prv_assert_event(0, AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonTimeout);
  cl_assert_equal_i(s_last_status_detail, AudioCompanionMuseCloseReasonAppFailed);
}

void test_audio_companion_muse__the_phone_link_dropping_closes(void) {
  prv_open_window();
  s_status = (PblBtMuseSetupStatus){0};
  s_event_handler(PblBtMuseSetupEventClosed, PblBtMuseSetupCloseReasonPhoneLinkLost);
  prv_run();
  prv_assert_event(0, AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonPhoneLinkLost);
  cl_assert_equal_i(s_last_status_detail, AudioCompanionMuseCloseReasonPhoneLinkLost);
}

void test_audio_companion_muse__leaving_the_card_cancels(void) {
  prv_open_window();
  audio_companion_muse_cancel_from_watch();
  prv_run();
  cl_assert_equal_i(s_close_calls, 1);
  prv_assert_event(0, AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonCancelledOnWatch);
  cl_assert_equal_i(s_last_status_detail, AudioCompanionMuseCloseReasonCancelledOnWatch);
}

void test_audio_companion_muse__the_receiver_going_away_closes_and_clears(void) {
  prv_open_window();
  s_notify_control_ok = false;
  static const uint8_t a[] = {1, 2, 3};
  s_rx_handler(a, sizeof(a));
  prv_run();
  audio_companion_muse_receiver_gone();
  prv_run();
  cl_assert_equal_i(s_close_calls, 1);
  cl_assert(!s_status.open);
  cl_assert(!stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id));

  // Nothing stale is delivered to the next receiver.
  s_notify_control_ok = true;
  const AudioCompanionMuseSetupOpenMsg msg = prv_open_msg("F1A69C", 300);
  cl_assert_equal_i(audio_companion_muse_handle_open(&msg), AudioCompanionAckStatusOk);
  audio_companion_muse_consent_response(true);
  prv_run();
  cl_assert_equal_i(s_note_count, 1);
  prv_assert_event(0, AudioCompanionMuseEventOpened, 0);
}
