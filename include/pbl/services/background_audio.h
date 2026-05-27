/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
  BackgroundAudioStateDisabled = 0,
  BackgroundAudioStateEnabledIdle,
  BackgroundAudioStateStarting,
  BackgroundAudioStateRecordingStreaming,
  BackgroundAudioStateRecordingBuffering,
  BackgroundAudioStatePausedForConflict,
  BackgroundAudioStatePausedForPolicy,
  BackgroundAudioStateStopping,
  BackgroundAudioStateError,
} BackgroundAudioState;

void background_audio_init(void);
bool background_audio_is_supported(void);
bool background_audio_is_device_supported(void);
bool background_audio_is_phone_supported(void);
bool background_audio_is_enabled(void);
void background_audio_set_enabled(bool enabled);
void background_audio_apply_enabled(bool enabled);
BackgroundAudioState background_audio_get_state(void);
void background_audio_pause_for_conflict(void);
void background_audio_resume_after_conflict(void);
void background_audio_handle_comm_session_changed(void);
void background_audio_handle_inbound_msg(const uint8_t *data, size_t size);
