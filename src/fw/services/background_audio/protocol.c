/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/background_audio_private.h"

#include <string.h>

size_t background_audio_stream_start_msg_size(void) {
  return sizeof(BackgroundAudioStreamStartMsg);
}

size_t background_audio_stream_data_header_size(void) {
  return sizeof(BackgroundAudioStreamDataHeader);
}

size_t background_audio_stream_gap_msg_size(void) {
  return sizeof(BackgroundAudioStreamGapMsg);
}

size_t background_audio_stream_stop_msg_size(void) {
  return sizeof(BackgroundAudioStreamStopMsg);
}

bool background_audio_parse_checkpoint_msg(const uint8_t *data, size_t size,
                                           BackgroundAudioStreamCheckpointMsg *out) {
  if (!data || !out || size < sizeof(BackgroundAudioStreamCheckpointMsg)) {
    return false;
  }
  if (data[0] != BackgroundAudioMsgIdStreamCheckpoint) {
    return false;
  }
  memcpy(out, data, sizeof(BackgroundAudioStreamCheckpointMsg));
  return true;
}
