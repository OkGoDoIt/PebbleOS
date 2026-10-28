/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/services/audio_endpoint.h>

#include <kernel/pebble_tasks.h>

struct PebbleCommSessionEvent;

//! Streams the microphone of the foreground app to the phone.
//!
//! Audio is encoded with dictation's Speex encoder and sent over the audio endpoint in a
//! VoiceEndpointSessionTypeAudioStream session; the phone decodes it and hands it to the app's
//! companion. Only the app task may stream, only while it is in focus, and never from a
//! watchface. The stream stops on any focus loss, when dictation takes the mic, when the phone
//! ends it or disconnects, or when the app goes away. The OS mic banner is shown while the mic
//! runs.
//!
//! The values of both enums are shared with the applib API.

typedef enum MicStreamServiceStartResult {
  MicStreamServiceStartOk = 0,
  MicStreamServiceStartErrBusy,
  MicStreamServiceStartErrNotForeground,
  MicStreamServiceStartErrInvalidArgs,
  MicStreamServiceStartErrNoMemory,
  MicStreamServiceStartErrWatchface,
  MicStreamServiceStartErrUnavailable,
} MicStreamServiceStartResult;

typedef enum MicStreamServiceStopReason {
  MicStreamServiceStopReasonStopped = 0,
  MicStreamServiceStopReasonFocusLost,
  MicStreamServiceStopReasonPreempted,
  MicStreamServiceStopReasonPhone,
  MicStreamServiceStopReasonError,
} MicStreamServiceStopReason;

void mic_stream_service_init(void);

//! Requests a stream for `owner`. The mic starts once the phone accepts the session, reported
//! with MicStreamEventStarted; a refusal or setup timeout is reported as a Phone stop.
//! @param owner Task requesting the stream; only the foreground app is allowed
//! @param stream_id Optional; set to the id carried by this stream's events
MicStreamServiceStartResult mic_stream_service_start(PebbleTask owner, uint32_t *stream_id);

//! Stops the task's stream, at its request or because it is going away. No stop event is sent.
//! Safe when idle.
void mic_stream_service_stop(PebbleTask task);

//! @return true while streaming, including the setup handshake
bool mic_stream_service_is_active(void);

//! The app lost focus to a modal window.
void mic_stream_service_handle_app_focus_lost(void);

//! Dictation is about to take the mic, the encoder and the phone-side audio session.
void mic_stream_service_handle_system_preempt(void);

//! The phone's answer to the setup of audio endpoint session `session_id`. Called by the voice
//! endpoint; answers for any other session are ignored.
void mic_stream_service_handle_setup_result(AudioEndpointSessionId session_id,
                                            uint8_t voice_endpoint_result);

//! A comm session opened or closed. Losing the phone ends the stream.
void mic_stream_service_handle_comm_session_event(const struct PebbleCommSessionEvent *event);
