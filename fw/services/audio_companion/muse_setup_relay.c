/* SPDX-License-Identifier: Apache-2.0 */

#include "muse_setup_relay.h"

#include <pbl/bluetooth/audio_companion.h>
#include <pbl/bluetooth/muse_setup.h>
#include "pbl/kernel/mutex.h"
#include "pbl/logging/logging.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/system_task.h"
#include "pbl/util/math.h"

#include <string.h>

PBL_LOG_MODULE_DECLARE(service_audio_companion, CONFIG_SERVICE_AUDIO_COMPANION_LOG_LEVEL);

// Everything the Bluetooth side reports (values the Muse app wrote, link events) goes through one
// byte ring, drained on the system task in order. The host task only ever appends. A setup
// exchange is a few hundred bytes in total, so the ring is small; a misbehaving client fills it
// and sees its writes dropped (counted, reported), never a blocked host task.
#define RING_BYTES (1024)
#define RECORD_HEADER_BYTES (6)
#define RECORD_KIND_RX (1)
#define RECORD_KIND_EVENT (2)
//! Pause before retrying a notification the transport refused.
#define BUSY_RETRY_MS (60)
//! Refusals in a row before the queue is dropped (~6 s): the receiver is gone, or not listening.
#define MAX_RETRIES (100)

typedef enum {
  RelayStateIdle,
  RelayStateConsentPending,
  RelayStateOpen,
} RelayState;

static PBL_MUTEX_DEFINE(s_lock);
static RelayState s_state;
static AudioCompanionMuseSetupOpenMsg s_open_request;
//! The reason the relay itself closed the window, so the Bluetooth side's generic "closed"
//! reports as what the app or the user actually did.
static uint8_t s_close_reason_override;
static bool s_consent_handled;
static AudioCompanionMuseConsentHandler s_consent_handler;
static AudioCompanionMuseStatusHandler s_status_handler;

static uint8_t s_ring[RING_BYTES];
static uint16_t s_ring_head;  // next byte to read
static uint16_t s_ring_used;
static bool s_drain_posted;
static uint8_t s_rx_seq;
//! Bytes of the record at the head already sent, for a value split across notifications.
static uint16_t s_head_sent;
static uint16_t s_rx_dropped;
static bool s_overflow_pending;
static TimerID s_retry_timer = TIMER_INVALID_ID;
static uint8_t s_retries;
//! The Muse app connected at least once during this window.
static bool s_client_seen;

// ---- ring (s_lock held) ----

static void prv_ring_write(uint16_t at, const uint8_t *data, uint16_t length) {
  for (uint16_t i = 0; i < length; i++) {
    s_ring[(at + i) % RING_BYTES] = data[i];
  }
}

static void prv_ring_read(uint16_t at, uint8_t *out, uint16_t length) {
  for (uint16_t i = 0; i < length; i++) {
    out[i] = s_ring[(at + i) % RING_BYTES];
  }
}

static bool prv_ring_push_locked(uint8_t kind, uint8_t a, uint16_t b, const uint8_t *payload,
                                 uint16_t length) {
  const uint16_t total = RECORD_HEADER_BYTES + length;
  if (s_ring_used + total > RING_BYTES) {
    return false;
  }
  const uint8_t header[RECORD_HEADER_BYTES] = {
    kind, a, (uint8_t)(b & 0xFF), (uint8_t)(b >> 8), (uint8_t)(length & 0xFF), (uint8_t)(length >> 8),
  };
  const uint16_t at = (s_ring_head + s_ring_used) % RING_BYTES;
  prv_ring_write(at, header, sizeof(header));
  if (length) {
    prv_ring_write((at + sizeof(header)) % RING_BYTES, payload, length);
  }
  s_ring_used += total;
  return true;
}

static void prv_ring_pop_locked(void) {
  uint8_t header[RECORD_HEADER_BYTES];
  prv_ring_read(s_ring_head, header, sizeof(header));
  const uint16_t length = (uint16_t)(header[4] | (header[5] << 8));
  const uint16_t total = RECORD_HEADER_BYTES + length;
  s_ring_head = (s_ring_head + total) % RING_BYTES;
  s_ring_used -= total;
  s_head_sent = 0;
}

// ---- draining (system task) ----

static void prv_drain_system_task_cb(void *data);

//! s_lock held.
static void prv_post_drain_locked(void) {
  if (s_drain_posted) {
    return;
  }
  s_drain_posted = true;
  system_task_add_callback(prv_drain_system_task_cb, NULL);
}

static void prv_retry_timer_cb(void *data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_post_drain_locked();
  pbl_mutex_unlock(&s_lock);
}

//! The transport refused a notification. s_lock NOT held.
static void prv_arm_retry(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (++s_retries > MAX_RETRIES) {
    // Nobody is taking notifications: drop what is queued rather than retry forever.
    PBL_LOG_WRN("Muse setup: receiver not taking notifications; dropped %u queued bytes",
                s_ring_used);
    s_ring_head = 0;
    s_ring_used = 0;
    s_head_sent = 0;
    s_overflow_pending = false;
    s_retries = 0;
    pbl_mutex_unlock(&s_lock);
    return;
  }
  pbl_mutex_unlock(&s_lock);
  if (s_retry_timer == TIMER_INVALID_ID) {
    s_retry_timer = new_timer_create();
  }
  new_timer_start(s_retry_timer, BUSY_RETRY_MS, prv_retry_timer_cb, NULL, 0);
}

static const char *prv_status_name(AudioCompanionMuseStatus status) {
  switch (status) {
    case AudioCompanionMuseStatusWaiting: return "waiting";
    case AudioCompanionMuseStatusConnected: return "connected";
    case AudioCompanionMuseStatusClosed: return "closed";
  }
  return "?";
}

static void prv_report_status(AudioCompanionMuseStatus status, uint8_t detail) {
  PBL_LOG_INFO("Muse setup: %s (%u)", prv_status_name(status), detail);
  if (s_status_handler) {
    s_status_handler(status, detail);
  }
}

//! Sends one event to the companion app, with the window's current MTU and time left.
static bool prv_send_event(uint8_t event, uint8_t detail) {
  PblBtMuseSetupStatus status;
  pbl_bt_muse_setup_get_status(&status);
  uint8_t buf[sizeof(AudioCompanionMuseSetupEventMsg)];
  const size_t len = audio_companion_protocol_build_muse_setup_event(
      buf, sizeof(buf), event, detail, status.mtu, (uint16_t)MIN(status.seconds_left, UINT16_MAX));
  return audio_companion_relay_notify_control(buf, len);
}

//! The largest MUSE_SETUP_RX payload the receiver's link takes in one notification.
static uint16_t prv_rx_chunk_bytes(void) {
  const uint16_t mtu = pbl_bt_audio_companion_get_effective_mtu();
  const uint16_t usable = mtu > 3 + sizeof(AudioCompanionMuseSetupRxHeader)
                              ? mtu - 3 - sizeof(AudioCompanionMuseSetupRxHeader)
                              : 20;
  return MIN(usable, AUDIO_COMPANION_MUSE_RX_MAX_BYTES);
}

static void prv_drain_system_task_cb(void *data) {
  while (true) {
    pbl_mutex_lock(&s_lock, PBL_FOREVER);
    if (s_overflow_pending) {
      // Reported ahead of the queue: the receiver should know bytes went missing before it
      // reads what came after them.
      const uint16_t dropped = s_rx_dropped;
      pbl_mutex_unlock(&s_lock);
      if (!prv_send_event(AudioCompanionMuseEventRxOverflow, (uint8_t)MIN(dropped, UINT8_MAX))) {
        pbl_mutex_lock(&s_lock, PBL_FOREVER);
        s_drain_posted = false;
        pbl_mutex_unlock(&s_lock);
        prv_arm_retry();
        return;
      }
      pbl_mutex_lock(&s_lock, PBL_FOREVER);
      s_overflow_pending = false;
      s_rx_dropped = 0;
      s_retries = 0;
    }
    if (s_ring_used == 0) {
      s_drain_posted = false;
      pbl_mutex_unlock(&s_lock);
      return;
    }
    uint8_t header[RECORD_HEADER_BYTES];
    prv_ring_read(s_ring_head, header, sizeof(header));
    const uint8_t kind = header[0];
    const uint8_t a = header[1];
    const uint16_t b = (uint16_t)(header[2] | (header[3] << 8));
    const uint16_t length = (uint16_t)(header[4] | (header[5] << 8));
    bool sent = false;
    if (kind == RECORD_KIND_RX) {
      const uint16_t chunk = (uint16_t)MIN(prv_rx_chunk_bytes(), length - s_head_sent);
      uint8_t payload[AUDIO_COMPANION_MUSE_RX_MAX_BYTES];
      prv_ring_read((s_ring_head + RECORD_HEADER_BYTES + s_head_sent) % RING_BYTES, payload, chunk);
      const bool more = s_head_sent + chunk < length;
      pbl_mutex_unlock(&s_lock);
      uint8_t buf[sizeof(AudioCompanionMuseSetupRxHeader) + AUDIO_COMPANION_MUSE_RX_MAX_BYTES];
      const size_t len = audio_companion_protocol_build_muse_setup_rx(
          buf, sizeof(buf), a, more ? AUDIO_COMPANION_MUSE_RX_FLAG_MORE : 0, payload,
          (uint8_t)chunk);
      sent = audio_companion_relay_notify_control(buf, len);
      pbl_mutex_lock(&s_lock, PBL_FOREVER);
      if (sent) {
        s_head_sent += chunk;
        if (s_head_sent >= length) {
          prv_ring_pop_locked();
        }
      }
    } else {
      // An event. The window's own close reason may be one the relay knows better.
      uint8_t detail = (uint8_t)b;
      if (a == AudioCompanionMuseEventClosed && s_close_reason_override) {
        detail = s_close_reason_override;
      }
      pbl_mutex_unlock(&s_lock);
      sent = prv_send_event(a, detail);
      pbl_mutex_lock(&s_lock, PBL_FOREVER);
      if (sent) {
        prv_ring_pop_locked();
        if (a == AudioCompanionMuseEventClosed) {
          s_close_reason_override = 0;
        }
      }
    }
    if (!sent) {
      // Out of transport buffers, or the receiver is gone. Keep the order; try again shortly
      // (a receiver that is gone takes the queue with it, see audio_companion_muse_receiver_gone).
      s_drain_posted = false;
      pbl_mutex_unlock(&s_lock);
      prv_arm_retry();
      return;
    }
    s_retries = 0;
    pbl_mutex_unlock(&s_lock);
  }
}

// ---- Bluetooth-side callbacks (host task, Bluetooth lock held: queue only) ----

static void prv_status_system_task_cb(void *data) {
  const uintptr_t packed = (uintptr_t)data;
  prv_report_status((AudioCompanionMuseStatus)(packed >> 8), (uint8_t)(packed & 0xFF));
}

//! s_lock held.
static void prv_post_status_locked(AudioCompanionMuseStatus status, uint8_t detail) {
  system_task_add_callback(prv_status_system_task_cb,
                           (void *)(uintptr_t)(((uintptr_t)status << 8) | detail));
}

static void prv_rx_from_host(const uint8_t *data, size_t length) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_state != RelayStateOpen) {
    // A write after the window closed, or before it opened: nobody is listening for it.
    pbl_mutex_unlock(&s_lock);
    return;
  }
  if (length > UINT8_MAX ||
      !prv_ring_push_locked(RECORD_KIND_RX, s_rx_seq, 0, data, (uint16_t)length)) {
    s_rx_dropped++;
    s_overflow_pending = true;
  }
  // Every value takes a sequence number, dropped or not, so the receiver sees the gap.
  s_rx_seq++;
  prv_post_drain_locked();
  pbl_mutex_unlock(&s_lock);
}

static void prv_event_from_host(PblBtMuseSetupEvent event, uint16_t detail) {
  uint8_t wire_event;
  switch (event) {
    // The MTU travels in every event's own field; these two carry no detail.
    case PblBtMuseSetupEventClientConnected:
      wire_event = AudioCompanionMuseEventClientConnected;
      detail = 0;
      break;
    case PblBtMuseSetupEventMtu:
      wire_event = AudioCompanionMuseEventMtu;
      detail = 0;
      break;
    case PblBtMuseSetupEventSubscribed: wire_event = AudioCompanionMuseEventSubscribed; break;
    case PblBtMuseSetupEventClientDisconnected:
      wire_event = AudioCompanionMuseEventClientDisconnected;
      break;
    case PblBtMuseSetupEventClosed: {
      wire_event = AudioCompanionMuseEventClosed;
      switch ((PblBtMuseSetupCloseReason)detail) {
        case PblBtMuseSetupCloseReasonTimeout: detail = AudioCompanionMuseCloseReasonTimeout; break;
        case PblBtMuseSetupCloseReasonPhoneLinkLost:
          detail = AudioCompanionMuseCloseReasonPhoneLinkLost;
          break;
        case PblBtMuseSetupCloseReasonError: detail = AudioCompanionMuseCloseReasonCouldNotStart; break;
        case PblBtMuseSetupCloseReasonClosed:
        default:
          detail = AudioCompanionMuseCloseReasonAppCancelled;  // replaced by the override
          break;
      }
      break;
    }
    default:
      return;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  // What the watch shows. A close the relay asked for was already reported by prv_close_window.
  if (s_state == RelayStateOpen) {
    switch (event) {
      case PblBtMuseSetupEventClientConnected:
        s_client_seen = true;
        prv_post_status_locked(AudioCompanionMuseStatusConnected, 0);
        break;
      case PblBtMuseSetupEventClientDisconnected:
        prv_post_status_locked(AudioCompanionMuseStatusWaiting, 0);
        break;
      case PblBtMuseSetupEventClosed:
        if (!s_close_reason_override) {
          // Timing out after the Muse app connected reads as a pairing that didn't finish.
          const bool unfinished = detail == AudioCompanionMuseCloseReasonTimeout && s_client_seen;
          prv_post_status_locked(AudioCompanionMuseStatusClosed,
                                 unfinished ? AudioCompanionMuseCloseReasonAppFailed
                                            : (uint8_t)detail);
        }
        break;
      default:
        break;
    }
  }
  if (event == PblBtMuseSetupEventClosed) {
    s_state = RelayStateIdle;
  }
  if (!prv_ring_push_locked(RECORD_KIND_EVENT, wire_event, detail, NULL, 0)) {
    PBL_LOG_WRN("Muse setup: event %u lost, ring full", wire_event);
  }
  prv_post_drain_locked();
  pbl_mutex_unlock(&s_lock);
}

// ---- window lifecycle (system task) ----

//! s_lock NOT held: calls into the Bluetooth service.
static void prv_close_window(uint8_t reason) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const RelayState state = s_state;
  if (state == RelayStateOpen) {
    s_close_reason_override = reason;
  } else if (state == RelayStateConsentPending) {
    s_state = RelayStateIdle;
    s_consent_handled = true;
    prv_ring_push_locked(RECORD_KIND_EVENT, AudioCompanionMuseEventClosed, reason, NULL, 0);
    prv_post_drain_locked();
  }
  pbl_mutex_unlock(&s_lock);
  if (state == RelayStateOpen) {
    // The service reports Closed through the event handler, which queues it with the override.
    pbl_bt_muse_setup_close();
  }
  if (state != RelayStateIdle) {
    prv_report_status(AudioCompanionMuseStatusClosed, reason);
  }
}

static void prv_consent_system_task_cb(void *data) {
  const bool granted = data != NULL;
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_state != RelayStateConsentPending || s_consent_handled) {
    pbl_mutex_unlock(&s_lock);
    return;
  }
  s_consent_handled = true;
  const AudioCompanionMuseSetupOpenMsg request = s_open_request;
  pbl_mutex_unlock(&s_lock);

  if (!granted) {
    prv_close_window(AudioCompanionMuseCloseReasonDeclinedOnWatch);
    return;
  }

  char suffix[PBL_BT_MUSE_SETUP_SUFFIX_LEN];
  memcpy(suffix, request.suffix, sizeof(suffix));
  const uint32_t secs = MIN(MAX((uint32_t)request.window_secs, AUDIO_COMPANION_MUSE_WINDOW_MIN_SECS),
                            AUDIO_COMPANION_MUSE_WINDOW_MAX_SECS);
  const bool opened = pbl_bt_muse_setup_open(suffix, secs * 1000);
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (opened) {
    s_state = RelayStateOpen;
    s_ring_head = 0;
    s_ring_used = 0;
    s_head_sent = 0;
    s_rx_seq = 0;
    s_rx_dropped = 0;
    s_overflow_pending = false;
    s_close_reason_override = 0;
    s_retries = 0;
    s_client_seen = false;
    prv_ring_push_locked(RECORD_KIND_EVENT, AudioCompanionMuseEventOpened, 0, NULL, 0);
  } else {
    s_state = RelayStateIdle;
    prv_ring_push_locked(RECORD_KIND_EVENT, AudioCompanionMuseEventClosed,
                         AudioCompanionMuseCloseReasonCouldNotStart, NULL, 0);
  }
  prv_post_drain_locked();
  pbl_mutex_unlock(&s_lock);
  prv_report_status(opened ? AudioCompanionMuseStatusWaiting : AudioCompanionMuseStatusClosed,
                    opened ? 0 : AudioCompanionMuseCloseReasonCouldNotStart);
}

void audio_companion_muse_consent_response(bool granted) {
  system_task_add_callback(prv_consent_system_task_cb, granted ? (void *)1 : NULL);
}

static void prv_cancel_system_task_cb(void *data) {
  prv_close_window(AudioCompanionMuseCloseReasonCancelledOnWatch);
}

void audio_companion_muse_cancel_from_watch(void) {
  system_task_add_callback(prv_cancel_system_task_cb, NULL);
}

static void prv_receiver_gone_system_task_cb(void *data) {
  prv_close_window(AudioCompanionMuseCloseReasonReceiverGone);
  // Nothing left to deliver to: the queue would only retry forever.
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_ring_head = 0;
  s_ring_used = 0;
  s_head_sent = 0;
  s_overflow_pending = false;
  if (s_retry_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_retry_timer);
  }
  pbl_mutex_unlock(&s_lock);
}

void audio_companion_muse_receiver_gone(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool active = s_state != RelayStateIdle || s_ring_used != 0;
  pbl_mutex_unlock(&s_lock);
  if (active) {
    system_task_add_callback(prv_receiver_gone_system_task_cb, NULL);
  }
}

// ---- requests from the companion app (service lock held) ----

static bool prv_valid_suffix(const char suffix[AUDIO_COMPANION_MUSE_SUFFIX_LEN]) {
  for (size_t i = 0; i < AUDIO_COMPANION_MUSE_SUFFIX_LEN; i++) {
    const char c = suffix[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

uint8_t audio_companion_muse_handle_open(const AudioCompanionMuseSetupOpenMsg *msg) {
  if (!prv_valid_suffix(msg->suffix)) {
    return AudioCompanionAckStatusRejected;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_state != RelayStateIdle) {
    pbl_mutex_unlock(&s_lock);
    return AudioCompanionAckStatusBadState;
  }
  if (!s_consent_handler) {
    pbl_mutex_unlock(&s_lock);
    return AudioCompanionAckStatusRejected;  // no UI to ask the user: fail closed
  }
  s_state = RelayStateConsentPending;
  s_consent_handled = false;
  s_open_request = *msg;
  pbl_mutex_unlock(&s_lock);
  PBL_LOG_INFO("Muse setup: pairing window requested (%u s)", msg->window_secs);
  s_consent_handler();
  return AudioCompanionAckStatusOk;
}

uint8_t audio_companion_muse_handle_close(uint8_t app_reason) {
  uint8_t reason;
  switch (app_reason) {
    case AudioCompanionMuseAppCloseReasonPaired: reason = AudioCompanionMuseCloseReasonAppPaired; break;
    case AudioCompanionMuseAppCloseReasonFailed: reason = AudioCompanionMuseCloseReasonAppFailed; break;
    default: reason = AudioCompanionMuseCloseReasonAppCancelled; break;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool active = s_state != RelayStateIdle;
  pbl_mutex_unlock(&s_lock);
  if (!active) {
    return AudioCompanionAckStatusBadState;
  }
  prv_close_window(reason);
  return AudioCompanionAckStatusOk;
}

uint8_t audio_companion_muse_handle_tx(const uint8_t *data, size_t length) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool open = s_state == RelayStateOpen;
  pbl_mutex_unlock(&s_lock);
  if (!open) {
    return AudioCompanionAckStatusBadState;
  }
  PblBtMuseSetupStatus status;
  pbl_bt_muse_setup_get_status(&status);
  if (!status.connected || !status.subscribed) {
    return AudioCompanionAckStatusRejected;
  }
  return pbl_bt_muse_setup_notify(data, length) ? AudioCompanionAckStatusOk
                                                 : AudioCompanionAckStatusBusy;
}

uint8_t audio_companion_muse_handle_disconnect(uint16_t delay_ms) {
  // The delay is the reference gadget's courtesy to the app's last read; the link ends now.
  (void)delay_ms;
  return pbl_bt_muse_setup_disconnect_client() ? AudioCompanionAckStatusOk
                                               : AudioCompanionAckStatusBadState;
}

// ---- setup ----

void audio_companion_muse_set_ui_handlers(AudioCompanionMuseConsentHandler consent,
                                          AudioCompanionMuseStatusHandler status) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_consent_handler = consent;
  s_status_handler = status;
  pbl_mutex_unlock(&s_lock);
}

void audio_companion_muse_init(void) {
  pbl_bt_muse_setup_set_rx_handler(prv_rx_from_host);
  pbl_bt_muse_setup_set_event_handler(prv_event_from_host);
}

void audio_companion_muse_test_reset(void) {
  pbl_mutex_init(&s_lock);
  s_state = RelayStateIdle;
  memset(&s_open_request, 0, sizeof(s_open_request));
  s_close_reason_override = 0;
  s_consent_handled = false;
  s_consent_handler = NULL;
  s_status_handler = NULL;
  s_ring_head = 0;
  s_ring_used = 0;
  s_drain_posted = false;
  s_rx_seq = 0;
  s_head_sent = 0;
  s_rx_dropped = 0;
  s_overflow_pending = false;
  s_retry_timer = TIMER_INVALID_ID;
  s_retries = 0;
  s_client_seen = false;
}
