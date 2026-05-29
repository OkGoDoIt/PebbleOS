/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/app_audio_context_private.h"

size_t app_audio_context_header_size(void) {
  return sizeof(AppAudioContextHeader);
}

size_t app_audio_context_status_response_msg_size(void) {
  return sizeof(AppAudioContextStatusResponseMsg);
}

size_t app_audio_context_transcript_response_header_size(void) {
  return sizeof(AppAudioContextTranscriptResponseHeader);
}
