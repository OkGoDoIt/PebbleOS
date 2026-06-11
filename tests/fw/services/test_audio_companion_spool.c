/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/audio_companion_private.h"

#include "services/audio_companion/spool.h"

#include "stubs_logging.h"
#include "stubs_passert.h"
#include "fake_pbl_malloc.h"

#include <string.h>

// Spool geometry used below: chunks are 4096-byte allocations holding packed
// records of (14-byte header + payload). With 50-byte payloads each record is
// 64 bytes, so one chunk holds 63 frames. CONFIG defaults in host tests are
// min 8192 (2 chunks), max 65536 (16 chunks), reserve 32768.

#define TEST_PAYLOAD_LEN (50)
#define FRAMES_PER_CHUNK (63)
#define MAX_CHUNKS (16)

#define DATA_HEADER_SIZE (sizeof(AudioCompanionStreamDataHeader))

static uint64_t prv_sample_index(uint32_t seq) {
  return (uint64_t)seq * AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES;
}

static void prv_push_one(uint32_t seq) {
  uint8_t payload[TEST_PAYLOAD_LEN];
  memset(payload, (uint8_t)seq, sizeof(payload));
  audio_companion_spool_push(seq, prv_sample_index(seq), payload, sizeof(payload));
}

static void prv_push_range(uint32_t first_seq, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    prv_push_one(first_seq + i);
  }
}

//! Drains one batch (up to max_message_bytes) and returns the frame count.
static uint8_t prv_drain_batch(size_t max_message_bytes, uint32_t *out_first_seq) {
  uint32_t first_seq = 0;
  uint64_t first_sample = 0;
  uint8_t frame_count = 0;
  uint8_t payload[4096];
  size_t payload_len = 0;
  if (!audio_companion_spool_peek_batch(max_message_bytes, DATA_HEADER_SIZE, &first_seq,
                                        &first_sample, &frame_count, payload, sizeof(payload),
                                        &payload_len)) {
    return 0;
  }
  // Validate framing and content of every entry in the batch.
  size_t offset = 0;
  for (uint8_t i = 0; i < frame_count; i++) {
    uint16_t len;
    memcpy(&len, &payload[offset], sizeof(len));
    cl_assert_equal_i(len, TEST_PAYLOAD_LEN);
    offset += sizeof(len);
    cl_assert_equal_i(payload[offset], (uint8_t)(first_seq + i));
    offset += len;
  }
  cl_assert_equal_i(offset, payload_len);
  cl_assert(first_sample == prv_sample_index(first_seq));
  audio_companion_spool_mark_sent_through(first_seq + frame_count - 1);
  if (out_first_seq) {
    *out_first_seq = first_seq;
  }
  return frame_count;
}

void test_audio_companion_spool__initialize(void) {
  audio_companion_spool_test_set_heap_free_bytes(UINT32_MAX);
  audio_companion_spool_init();
}

void test_audio_companion_spool__cleanup(void) {
  audio_companion_spool_deinit();
  fake_pbl_malloc_check_net_allocs();
}

void test_audio_companion_spool__push_then_drain_in_order(void) {
  prv_push_range(0, 10);
  cl_assert_equal_i(audio_companion_spool_frames_pending_send(), 10);

  uint32_t first_seq = 0;
  uint8_t count = prv_drain_batch(1024, &first_seq);
  cl_assert_equal_i(first_seq, 0);
  cl_assert(count > 0 && count <= 10);
  uint8_t total = count;
  while ((count = prv_drain_batch(1024, &first_seq)) > 0) {
    cl_assert_equal_i(first_seq, total);
    total += count;
  }
  cl_assert_equal_i(total, 10);
  cl_assert_equal_i(audio_companion_spool_frames_pending_send(), 0);

  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert_equal_i(stats.pushed_frames, 10);
  cl_assert_equal_i(stats.frames_queued, 10);  // sent but not yet checkpointed
  cl_assert_equal_i(stats.dropped_overflow_frames, 0);
}

void test_audio_companion_spool__batch_respects_message_size_and_count_caps(void) {
  prv_push_range(0, 64);

  // Room for exactly 3 entries of (2 + 50) bytes after the 20-byte header.
  uint8_t count = prv_drain_batch(DATA_HEADER_SIZE + 3 * (2 + TEST_PAYLOAD_LEN) + 1, NULL);
  cl_assert_equal_i(count, 3);

  // A huge budget is still capped at AUDIO_COMPANION_MAX_FRAMES_PER_DATA_MSG.
  count = prv_drain_batch(1 << 20, NULL);
  cl_assert_equal_i(count, AUDIO_COMPANION_MAX_FRAMES_PER_DATA_MSG);
}

void test_audio_companion_spool__checkpoint_trim_releases_and_drain_survives(void) {
  prv_push_range(0, 100);

  uint32_t first_seq = 0;
  uint8_t count = prv_drain_batch(1 << 20, &first_seq);  // sends 0..31
  cl_assert_equal_i(count, 32);

  // Receiver checkpoints through 20; frames 21..31 stay for possible resend.
  audio_companion_spool_trim_through(20);
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert_equal_i(stats.frames_queued, 79);

  // Next drain continues at 32 (trim must not disturb the cursor).
  count = prv_drain_batch(1 << 20, &first_seq);
  cl_assert_equal_i(first_seq, 32);
  cl_assert_equal_i(count, 32);
}

void test_audio_companion_spool__rewind_resends_unacked_frames(void) {
  prv_push_range(0, 40);
  uint32_t first_seq = 0;
  cl_assert_equal_i(prv_drain_batch(1 << 20, &first_seq), 32);  // 0..31 sent
  audio_companion_spool_trim_through(15);                       // 0..15 persisted

  // Reconnect: everything not checkpointed must be resent, starting at 16.
  audio_companion_spool_rewind_unsent();
  cl_assert_equal_i(audio_companion_spool_frames_pending_send(), 24);
  cl_assert_equal_i(prv_drain_batch(1 << 20, &first_seq), 24);
  cl_assert_equal_i(first_seq, 16);
}

void test_audio_companion_spool__overflow_drops_oldest_and_merges_gap(void) {
  // Cap the spool at its floor: heap has no headroom beyond the reserve.
  audio_companion_spool_test_set_heap_free_bytes(0);

  // Floor is 2 chunks = 126 frames; push 200 so 74+ oldest frames must drop.
  prv_push_range(0, 200);

  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert(stats.current_bytes <= 8192);
  cl_assert(stats.dropped_overflow_frames >= 74);

  AudioCompanionSpoolPendingGap gap;
  cl_assert(audio_companion_spool_take_pending_gap(&gap));
  cl_assert_equal_i(gap.reason, AudioCompanionGapReasonSpoolOverflow);
  cl_assert_equal_i(gap.first_missing_sequence, 0);
  cl_assert(gap.first_missing_sample_index == 0);
  cl_assert_equal_i(gap.missing_frame_count, stats.dropped_overflow_frames);
  cl_assert(!audio_companion_spool_has_pending_gap());

  // The retained window is the *newest* audio ending at sequence 199.
  uint32_t first_seq = 0;
  cl_assert(prv_drain_batch(1 << 20, &first_seq) > 0);
  cl_assert_equal_i(first_seq, gap.first_missing_sequence + gap.missing_frame_count);
  uint8_t count;
  uint32_t last_first = first_seq;
  while ((count = prv_drain_batch(1 << 20, &last_first)) > 0) {
    first_seq = last_first + count;
  }
  cl_assert_equal_i(first_seq, 200);
}

void test_audio_companion_spool__grows_only_with_heap_headroom(void) {
  // Plenty of heap: the spool may grow past the floor toward the ceiling.
  audio_companion_spool_test_set_heap_free_bytes(UINT32_MAX);
  prv_push_range(0, FRAMES_PER_CHUNK * 4);
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert(stats.current_bytes > 8192);
  cl_assert_equal_i(stats.dropped_overflow_frames, 0);

  // Heap pressure: growth stops, pushes recycle the oldest chunk instead.
  audio_companion_spool_test_set_heap_free_bytes(0);
  const uint32_t bytes_before = stats.current_bytes;
  prv_push_range(FRAMES_PER_CHUNK * 4, FRAMES_PER_CHUNK * 2);
  audio_companion_spool_get_stats(&stats);
  cl_assert_equal_i(stats.current_bytes, bytes_before);
  cl_assert(stats.dropped_overflow_frames > 0);
  cl_assert(audio_companion_spool_has_pending_gap());
}

void test_audio_companion_spool__never_exceeds_ceiling(void) {
  audio_companion_spool_test_set_heap_free_bytes(UINT32_MAX);
  prv_push_range(0, FRAMES_PER_CHUNK * (MAX_CHUNKS + 4));
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert(stats.current_bytes <= 65536);
  cl_assert(stats.high_water_bytes <= 65536);
  cl_assert(stats.dropped_overflow_frames > 0);
}

void test_audio_companion_spool__trim_frees_emptied_chunks(void) {
  audio_companion_spool_test_set_heap_free_bytes(UINT32_MAX);
  prv_push_range(0, FRAMES_PER_CHUNK * 3);
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  const uint32_t grown_bytes = stats.current_bytes;
  cl_assert(grown_bytes >= 3 * 4096);

  // Drain and checkpoint everything: chunks must be returned to the heap.
  uint32_t first_seq = 0;
  while (prv_drain_batch(1 << 20, &first_seq) > 0) {
  }
  audio_companion_spool_trim_through(FRAMES_PER_CHUNK * 3 - 1);
  audio_companion_spool_get_stats(&stats);
  cl_assert_equal_i(stats.frames_queued, 0);
  cl_assert(stats.current_bytes < grown_bytes);

  // The spool must still accept and deliver new frames afterwards.
  prv_push_range(1000, 5);
  cl_assert_equal_i(prv_drain_batch(1 << 20, &first_seq), 5);
  cl_assert_equal_i(first_seq, 1000);
}

void test_audio_companion_spool__explicit_gap_records_merge(void) {
  audio_companion_spool_record_gap(10, 5, prv_sample_index(10),
                                   AudioCompanionGapReasonMicConflict);
  // A later overflow while a gap is pending merges into the same record.
  audio_companion_spool_record_gap(15, 3, prv_sample_index(15),
                                   AudioCompanionGapReasonSpoolOverflow);

  AudioCompanionSpoolPendingGap gap;
  cl_assert(audio_companion_spool_take_pending_gap(&gap));
  cl_assert_equal_i(gap.first_missing_sequence, 10);
  cl_assert_equal_i(gap.missing_frame_count, 8);
  cl_assert_equal_i(gap.reason, AudioCompanionGapReasonMicConflict);

  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert_equal_i(stats.gap_records, 1);
}

void test_audio_companion_spool__variable_length_payloads_roundtrip(void) {
  // Mix of sizes incl. the maximum; validates packed record framing.
  const uint16_t lengths[] = { 1, 25, 100, AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES, 7 };
  uint8_t payload[AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES];
  for (uint32_t i = 0; i < 5; i++) {
    memset(payload, (uint8_t)(0xA0 + i), lengths[i]);
    audio_companion_spool_push(i, prv_sample_index(i), payload, lengths[i]);
  }

  uint32_t first_seq = 0;
  uint64_t first_sample = 0;
  uint8_t frame_count = 0;
  uint8_t buf[2048];
  size_t buf_len = 0;
  cl_assert(audio_companion_spool_peek_batch(1 << 20, DATA_HEADER_SIZE, &first_seq,
                                             &first_sample, &frame_count, buf, sizeof(buf),
                                             &buf_len));
  cl_assert_equal_i(frame_count, 5);
  size_t offset = 0;
  for (uint32_t i = 0; i < 5; i++) {
    uint16_t len;
    memcpy(&len, &buf[offset], sizeof(len));
    cl_assert_equal_i(len, lengths[i]);
    offset += sizeof(len);
    cl_assert_equal_i(buf[offset], 0xA0 + i);
    cl_assert_equal_i(buf[offset + len - 1], 0xA0 + i);
    offset += len;
  }
  cl_assert_equal_i(offset, buf_len);
}

void test_audio_companion_spool__reset_clears_everything(void) {
  prv_push_range(0, 100);
  audio_companion_spool_record_gap(5, 2, 0, AudioCompanionGapReasonMicConflict);
  audio_companion_spool_reset();

  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  cl_assert_equal_i(stats.pushed_frames, 0);
  cl_assert_equal_i(stats.frames_queued, 0);
  cl_assert_equal_i(stats.current_bytes, 0);
  cl_assert(!audio_companion_spool_has_pending_gap());
  cl_assert_equal_i(audio_companion_spool_frames_pending_send(), 0);
}
