/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/background_audio_private.h"

void test_background_audio_protocol__sizes(void) {
  cl_assert_equal_i(background_audio_stream_start_msg_size(), sizeof(BackgroundAudioStreamStartMsg));
  cl_assert_equal_i(background_audio_stream_gap_msg_size(), sizeof(BackgroundAudioStreamGapMsg));
  cl_assert_equal_i(background_audio_stream_stop_msg_size(), sizeof(BackgroundAudioStreamStopMsg));
}

void test_background_audio_protocol__checkpoint_parse(void) {
  BackgroundAudioStreamCheckpointMsg written = {
    .command_id = BackgroundAudioMsgIdStreamCheckpoint,
    .stream_id = 42,
    .highest_contiguous_sequence_persisted = 99,
    .persisted_sample_index = 12345,
    .receiver_flags = 1,
    .free_storage_hint_kb = 1024,
  };
  BackgroundAudioStreamCheckpointMsg parsed;
  cl_assert(background_audio_parse_checkpoint_msg((const uint8_t *)&written, sizeof(written),
                                                  &parsed));
  cl_assert_equal_i(parsed.stream_id, 42);
  cl_assert_equal_i(parsed.highest_contiguous_sequence_persisted, 99);
}
