/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "../../../src/fw/services/background_audio/spool.h"
#include "pbl/services/background_audio_private.h"
#include "stubs_passert.h"

void test_background_audio_spool__initialize(void) {
  background_audio_spool_init();
}

void test_background_audio_spool__push_pop_order(void) {
  background_audio_spool_reset();
  const uint8_t payload_a[] = {0x01, 0x02};
  const uint8_t payload_b[] = {0x03, 0x04, 0x05};

  cl_assert(background_audio_spool_push(1, 0, payload_a, sizeof(payload_a), 0));
  cl_assert(background_audio_spool_push(2, 320, payload_b, sizeof(payload_b), 0));
  cl_assert_equal_i(background_audio_spool_depth(), 2);

  uint32_t first_sequence = 0;
  uint64_t first_sample_index = 0;
  uint8_t frame_count = 0;
  uint8_t payload_buf[64];
  size_t payload_len = 0;

  cl_assert(background_audio_spool_peek_batch(512, &first_sequence, &first_sample_index,
                                             &frame_count, payload_buf, sizeof(payload_buf),
                                             &payload_len));
  cl_assert_equal_i(first_sequence, 1);
  cl_assert_equal_i(frame_count, 2);

  background_audio_spool_pop_through(2);
  cl_assert_equal_i(background_audio_spool_depth(), 0);
}

void test_background_audio_spool__overflow_records_gap(void) {
  background_audio_spool_reset();
  uint8_t payload[4] = {0};
  for (uint32_t i = 0; i < 520; i++) {
    background_audio_spool_push(i, i * 320, payload, sizeof(payload), 0);
  }
  cl_assert(background_audio_spool_dropped_overflow_count() > 0);
  cl_assert(background_audio_spool_has_pending_gap());
}
