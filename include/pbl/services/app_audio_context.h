/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "applib/audio_context.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

void app_audio_context_init(void);

AudioContextAvailability app_audio_context_get_cached_status(AudioContextStatus *status_out);

uint16_t app_audio_context_next_request_id(void);

bool app_audio_context_request_status(uint16_t request_id);

bool app_audio_context_request_enable(uint16_t request_id);

bool app_audio_context_request_permission(uint16_t request_id, AudioContextPermission permissions);

bool app_audio_context_request_recent_transcript(uint16_t request_id, uint32_t before_seconds,
                                                 uint32_t after_seconds);

bool app_audio_context_request_transcript_history(uint16_t request_id, time_t start_time,
                                                  time_t end_time);

bool app_audio_context_subscribe_transcript(uint16_t request_id);

void app_audio_context_cancel(uint16_t request_id);
