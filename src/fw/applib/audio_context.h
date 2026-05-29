/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "applib/app_launch_button.h"
#include "applib/app_launch_reason.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

//! @addtogroup Foundation
//! @{
//!   @addtogroup AudioContext Audio Context
//!   \brief Permissioned access to system background audio context.
//!
//! Audio Context gives apps a permissioned view of audio data already captured
//! by the system background audio service. Apps can query status, request
//! recent transcripts, subscribe to transcript updates while active, and inspect
//! launch trigger metadata.
//!
//! Apps do not start or stop background recording with this API. The user
//! controls background audio in the system and grants access per app.
//!   @{

//! Availability or failure reason for Audio Context requests.
typedef enum {
  //! Audio Context is available.
  AudioContextAvailabilityAvailable = 0,
  //! The connected watch or firmware does not support Audio Context.
  AudioContextAvailabilityUnsupportedWatch,
  //! The connected phone app does not support Audio Context.
  AudioContextAvailabilityUnsupportedPhone,
  //! The user has disabled system background audio.
  AudioContextAvailabilityDisabledByUser,
  //! The user has not granted this app the requested audio access.
  AudioContextAvailabilityPermissionDenied,
  //! The app did not declare the required capability in `package.json`.
  AudioContextAvailabilityCapabilityNotDeclared,
  //! Transcription is not available for the requested audio data.
  AudioContextAvailabilityTranscriptionUnavailable,
  //! No transcript data is available for the requested time window.
  AudioContextAvailabilityNoData,
  //! An unexpected error occurred.
  AudioContextAvailabilityError,
} AudioContextAvailability;

//! Permission scopes an app can request for Audio Context.
typedef enum {
  //! Check Audio Context availability and background audio status.
  AudioContextPermissionStatus = 1 << 0,
  //! Read recent transcript context.
  AudioContextPermissionRecentTranscript = 1 << 1,
  //! Read bounded transcript history.
  AudioContextPermissionTranscriptHistory = 1 << 2,
  //! Subscribe to live transcript updates while the app is active.
  AudioContextPermissionLiveTranscript = 1 << 3,
  //! Receive raw phone-side audio. Not exposed to watch C apps in v1.
  AudioContextPermissionRawAudio = 1 << 4,
} AudioContextPermission;

//! Current status of system background audio as seen by the phone.
typedef struct {
  //! Availability of Audio Context for this app.
  AudioContextAvailability availability;
  //! True if background audio is enabled or has captured data.
  bool background_audio_enabled;
  //! True if the phone is currently receiving background audio.
  bool recording;
  //! True if transcription is available and enabled.
  bool transcribing;
  //! Reserved flags for future status details.
  uint32_t flags;
} AudioContextStatus;

//! A transcript segment returned by an Audio Context request.
typedef struct {
  //! Start time of the segment in seconds since the Unix epoch.
  time_t start_time;
  //! End time of the segment in seconds since the Unix epoch.
  time_t end_time;
  //! Number of known capture gaps associated with this segment.
  uint32_t gap_count;
  //! Reserved flags for future transcript details.
  uint32_t flags;
  //! UTF-8 transcript text. Valid only during the callback.
  const char *text;
} AudioContextTranscript;

//! Source category that caused an Audio Context-aware launch.
typedef enum {
  //! The launch source is unknown.
  AudioContextTriggerSourceUnknown = 0,
  //! The launch was triggered from the watch.
  AudioContextTriggerSourceWatch,
  //! The launch was triggered from the phone.
  AudioContextTriggerSourcePhone,
  //! The launch was triggered from a paired ring or similar accessory.
  AudioContextTriggerSourceRing,
  //! The launch was triggered from a shortcut.
  AudioContextTriggerSourceShortcut,
  //! The launch was triggered by the system.
  AudioContextTriggerSourceSystem,
} AudioContextTriggerSource;

//! Source-neutral metadata about how the app was launched.
typedef struct {
  //! Standard Pebble app launch reason.
  AppLaunchReason launch_reason;
  //! Button associated with the launch, if any.
  ButtonId launch_button;
  //! Source category for the launch or action.
  AudioContextTriggerSource source;
  //! Source-specific action identifier. Zero when unavailable.
  uint32_t source_action;
  //! Best-effort trigger time in seconds since the Unix epoch.
  time_t trigger_time;
  //! Standard Pebble launch arguments.
  uint32_t args;
} AudioContextTriggerInfo;

//! Callback for asynchronous Audio Context status requests.
//!
//! If `result` is not `AudioContextAvailabilityAvailable`, `status` may be
//! NULL. The callback runs on the app event loop.
typedef void (*AudioContextStatusCallback)(AudioContextAvailability result,
                                           const AudioContextStatus *status,
                                           void *context);

//! Callback for transcript query and subscription results.
//!
//! If `result` is not `AudioContextAvailabilityAvailable`, `transcript` may be
//! NULL. The `transcript` pointer and `transcript->text` are valid only for the
//! duration of the callback. Copy the text before returning if it is needed
//! later.
typedef void (*AudioContextTranscriptCallback)(AudioContextAvailability result,
                                               const AudioContextTranscript *transcript,
                                               void *context);

//! Gets the most recent cached Audio Context status.
//!
//! This function does not contact the phone. Before a status response has been
//! received, the cached availability is
//! `AudioContextAvailabilityUnsupportedPhone`.
//!
//! @param status_out Optional destination for the cached status.
//! @return The cached availability value.
AudioContextAvailability audio_context_get_cached_status(AudioContextStatus *status_out);

//! Requests fresh Audio Context status from the phone.
//!
//! Requires the `audio_status` capability and user permission. The callback
//! receives the status or an availability value describing why status could not
//! be returned.
//!
//! @param callback Callback invoked when the phone responds.
//! @param context Optional context pointer passed to `callback`.
//! @return True if the request was queued for delivery, false otherwise.
bool audio_context_request_status(AudioContextStatusCallback callback, void *context);

//! Asks the system to show the background audio enable flow.
//!
//! Apps cannot enable background audio directly. This function only requests
//! that the phone show the user an appropriate flow.
//!
//! @return True if the request was sent, false otherwise. This is not the
//! result of the user prompt.
bool audio_context_request_enable(void);

//! Asks the system to show the Audio Context permission flow.
//!
//! The app must declare the matching capabilities in `package.json`. This
//! function does not grant permissions directly and does not report the final
//! prompt result. Request status or transcript data afterwards to observe the
//! current permission state.
//!
//! @param permissions Bitmask of `AudioContextPermission` values.
//! @return True if the request was sent, false otherwise. This is not the
//! result of the user prompt.
bool audio_context_request_permission(AudioContextPermission permissions);

//! Requests recent transcript context around the current time.
//!
//! Requires the `audio_transcript` capability and user permission. The phone
//! may return no data if transcription is delayed or no segment overlaps the
//! requested window.
//!
//! @param before_seconds Seconds before the anchor time to include.
//! @param after_seconds Seconds after the anchor time to include.
//! @param callback Callback invoked with the transcript or an availability
//! value.
//! @param context Optional context pointer passed to `callback`.
//! @return True if the request was queued for delivery, false otherwise.
bool audio_context_request_recent_transcript(uint32_t before_seconds, uint32_t after_seconds,
                                             AudioContextTranscriptCallback callback,
                                             void *context);

//! Requests bounded transcript history.
//!
//! Requires the `audio_history` capability and user permission. Use small
//! windows on watch apps; larger history and cloud processing are better suited
//! to PebbleKit JS on the phone.
//!
//! @param start_time Start of the history window in seconds since the Unix
//! epoch.
//! @param end_time End of the history window in seconds since the Unix epoch.
//! @param callback Callback invoked with the transcript or an availability
//! value.
//! @param context Optional context pointer passed to `callback`.
//! @return True if the request was queued for delivery, false otherwise.
bool audio_context_request_transcript_history(time_t start_time, time_t end_time,
                                              AudioContextTranscriptCallback callback,
                                              void *context);

//! Gets source-neutral metadata about the current app launch.
//!
//! This is useful for Quick Launch or accessory-triggered workflows. Apps can
//! use the trigger time to decide which recent transcript window to request.
//!
//! @param info_out Destination for trigger metadata.
//! @return True if trigger metadata was written, false otherwise.
bool audio_context_get_trigger_info(AudioContextTriggerInfo *info_out);

//! Subscribes to live transcript updates while the app is active.
//!
//! Requires the `audio_transcript` capability and user permission. Only one
//! watch-side transcript subscription can be active through this API. Call
//! `audio_context_unsubscribe()` when updates are no longer needed.
//!
//! @param callback Callback invoked for each transcript update.
//! @param context Optional context pointer passed to `callback`.
//! @return True if the subscription request was queued, false otherwise.
bool audio_context_subscribe_transcript(AudioContextTranscriptCallback callback, void *context);

//! Cancels the active transcript subscription, if any.
void audio_context_unsubscribe(void);

//!   @} // group AudioContext
//! @} // group Foundation
