/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/audio_companion_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Audio companion background streaming service. Captures microphone audio,
//! encodes it with Speex, and streams it over a dedicated watch-hosted GATT
//! service to a single authorized third-party receiver app. Fail-closed by
//! design: nothing is captured unless the user enabled the feature, and
//! nothing is sent before the receiver authorizes on the current connection.

typedef struct {
  AudioCompanionServiceState state;
  uint32_t captured_frames;
  uint32_t sent_frames;
  uint32_t send_backpressure_events;
  uint32_t gap_records;
  uint32_t dropped_overflow_frames;
  uint32_t mic_conflicts;
  uint32_t spool_bytes;
  uint32_t spool_high_water_bytes;
  uint32_t loss_alerts_posted;
} AudioCompanionDiagnostics;

void audio_companion_init(void);

bool audio_companion_is_enabled(void);
//! Persists the pref and applies it to the running service.
void audio_companion_set_enabled(bool enabled);
//! Applies an already-persisted pref value (called from prefs restore).
void audio_companion_apply_enabled(bool enabled);

AudioCompanionServiceState audio_companion_get_state(void);
void audio_companion_get_diagnostics(AudioCompanionDiagnostics *diag_out);

bool audio_companion_get_receiver_name(char *buf, size_t buf_size);
//! User revoke from watch Settings: wipes identity, notifies the app, stops.
void audio_companion_forget_receiver(void);

//! Mic arbitration, called by the voice service. Dictation always wins.
void audio_companion_mic_conflict_begin(void);
void audio_companion_mic_conflict_end(void);

//! Consent UI integration: the UI layer registers a handler that shows the
//! on-watch prompt; with no handler registered, consent requests are denied
//! (fail closed). The handler must eventually call
//! audio_companion_handle_consent_response (a 60 s timeout declines).
//! Invoked on the system task with the service lock held: implementations
//! must defer UI work to KernelMain and must not call
//! audio_companion_handle_consent_response() synchronously.
typedef void (*AudioCompanionConsentHandler)(const char *receiver_name);
void audio_companion_set_consent_handler(AudioCompanionConsentHandler handler);
void audio_companion_handle_consent_response(bool granted);
