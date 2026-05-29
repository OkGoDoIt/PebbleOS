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
//!   \brief Permissioned access to recent background audio transcripts.
//!   @{

typedef enum {
  AudioContextAvailabilityAvailable = 0,
  AudioContextAvailabilityUnsupportedWatch,
  AudioContextAvailabilityUnsupportedPhone,
  AudioContextAvailabilityDisabledByUser,
  AudioContextAvailabilityPermissionDenied,
  AudioContextAvailabilityCapabilityNotDeclared,
  AudioContextAvailabilityTranscriptionUnavailable,
  AudioContextAvailabilityNoData,
  AudioContextAvailabilityError,
} AudioContextAvailability;

typedef enum {
  AudioContextPermissionStatus = 1 << 0,
  AudioContextPermissionRecentTranscript = 1 << 1,
  AudioContextPermissionTranscriptHistory = 1 << 2,
  AudioContextPermissionLiveTranscript = 1 << 3,
  AudioContextPermissionRawAudio = 1 << 4,
} AudioContextPermission;

typedef struct {
  AudioContextAvailability availability;
  bool background_audio_enabled;
  bool recording;
  bool transcribing;
  uint32_t flags;
} AudioContextStatus;

typedef struct {
  time_t start_time;
  time_t end_time;
  uint32_t gap_count;
  uint32_t flags;
  const char *text;
} AudioContextTranscript;

typedef enum {
  AudioContextTriggerSourceUnknown = 0,
  AudioContextTriggerSourceWatch,
  AudioContextTriggerSourcePhone,
  AudioContextTriggerSourceRing,
  AudioContextTriggerSourceShortcut,
  AudioContextTriggerSourceSystem,
} AudioContextTriggerSource;

typedef struct {
  AppLaunchReason launch_reason;
  ButtonId launch_button;
  AudioContextTriggerSource source;
  uint32_t source_action;
  time_t trigger_time;
  uint32_t args;
} AudioContextTriggerInfo;

typedef void (*AudioContextStatusCallback)(AudioContextAvailability result,
                                           const AudioContextStatus *status,
                                           void *context);

typedef void (*AudioContextTranscriptCallback)(AudioContextAvailability result,
                                               const AudioContextTranscript *transcript,
                                               void *context);

AudioContextAvailability audio_context_get_cached_status(AudioContextStatus *status_out);

bool audio_context_request_status(AudioContextStatusCallback callback, void *context);

bool audio_context_request_enable(void);

bool audio_context_request_permission(AudioContextPermission permissions);

bool audio_context_request_recent_transcript(uint32_t before_seconds, uint32_t after_seconds,
                                             AudioContextTranscriptCallback callback,
                                             void *context);

bool audio_context_request_transcript_history(time_t start_time, time_t end_time,
                                              AudioContextTranscriptCallback callback,
                                              void *context);

bool audio_context_get_trigger_info(AudioContextTriggerInfo *info_out);

bool audio_context_subscribe_transcript(AudioContextTranscriptCallback callback, void *context);

void audio_context_unsubscribe(void);

//!   @} // group AudioContext
//! @} // group Foundation
