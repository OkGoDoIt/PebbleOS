/* SPDX-License-Identifier: Apache-2.0 */

#include "spool.h"

#include "pbl/services/audio_companion_private.h"

#include "kernel/pbl_malloc.h"
#include "pbl/logging/logging.h"
#include "system/passert.h"
#include "pbl/util/attributes.h"

#ifndef UNITTEST
#include "kernel/kernel_heap.h"
#include "pbl/util/heap.h"
#endif

#include <string.h>

#ifndef CONFIG_AUDIO_COMPANION_SPOOL_MIN_BYTES
#define CONFIG_AUDIO_COMPANION_SPOOL_MIN_BYTES 8192
#endif
#ifndef CONFIG_AUDIO_COMPANION_SPOOL_MAX_BYTES
#define CONFIG_AUDIO_COMPANION_SPOOL_MAX_BYTES 131072
#endif
#ifndef CONFIG_AUDIO_COMPANION_SPOOL_HEAP_RESERVE_BYTES
#define CONFIG_AUDIO_COMPANION_SPOOL_HEAP_RESERVE_BYTES 32768
#endif

#define SPOOL_CHUNK_BYTES (4096)
//! Records waiting to be notified as STREAM_GAP. Silence-suppression, overflow and pause gaps
//! interleave while the link is stalled, so a handful of slots fills within seconds; 12 (384 B
//! of static RAM) keeps the last-resort fold below genuinely rare.
#define MAX_PENDING_GAPS (12)

typedef struct PACKED {
  uint32_t sequence;
  uint64_t sample_index;
  uint16_t length;
} SpoolRecordHeader;

_Static_assert(sizeof(SpoolRecordHeader) == 14, "spool record header size");

typedef struct SpoolChunk {
  struct SpoolChunk *next;
  uint16_t used;         //!< bytes written into data[]
  uint16_t trim_offset;  //!< bytes released by checkpoints
  uint8_t data[];
} SpoolChunk;

#define SPOOL_CHUNK_DATA_BYTES (SPOOL_CHUNK_BYTES - sizeof(SpoolChunk))

static SpoolChunk *s_head;
static SpoolChunk *s_tail;
static SpoolChunk *s_drain_chunk;
static uint16_t s_drain_offset;
static AudioCompanionSpoolPendingGap s_pending_gaps[MAX_PENDING_GAPS];
static uint8_t s_pending_gap_count;
static uint32_t s_pushed_frames;
static uint32_t s_dropped_overflow_frames;
static uint32_t s_gap_records;
static uint32_t s_gap_coalesced_frames;
static uint32_t s_frames_queued;
static uint32_t s_frames_unsent;
static uint32_t s_current_bytes;
static uint32_t s_high_water_bytes;

#ifdef UNITTEST
static uint32_t s_test_heap_free_bytes = UINT32_MAX;

void audio_companion_spool_test_set_heap_free_bytes(uint32_t bytes) {
  s_test_heap_free_bytes = bytes;
}

static uint32_t prv_heap_free_bytes(void) { return s_test_heap_free_bytes; }
#else
static uint32_t prv_heap_free_bytes(void) {
  Heap *heap = kernel_heap_get();
  const size_t size = heap_size(heap);
  return (size > heap->current_size) ? (uint32_t)(size - heap->current_size) : 0;
}
#endif

static const SpoolRecordHeader *prv_record_at(const SpoolChunk *chunk, uint16_t offset) {
  return (const SpoolRecordHeader *)&chunk->data[offset];
}

static uint16_t prv_record_size(const SpoolRecordHeader *header) {
  return sizeof(*header) + header->length;
}

static uint32_t prv_gap_end(const AudioCompanionSpoolPendingGap *gap) {
  return gap->first_missing_sequence + gap->missing_frame_count;
}

static void prv_remove_gap(uint8_t index) {
  if ((uint8_t)(index + 1) < s_pending_gap_count) {
    memmove(&s_pending_gaps[index], &s_pending_gaps[index + 1],
            (s_pending_gap_count - index - 1) * sizeof(s_pending_gaps[0]));
  }
  s_pending_gap_count--;
  memset(&s_pending_gaps[s_pending_gap_count], 0, sizeof(s_pending_gaps[0]));
}

//! Order records by sequence and fold neighbours whose ranges touch exactly and share a reason.
//! Order matters twice: the drain notifies records oldest-sequence first, and the last-resort
//! fold below is only meaningful between sequence neighbours. Folding here is loss-free -- the
//! union of two abutting ranges is exactly the sum of their frames.
static void prv_normalize_gaps(void) {
  for (uint8_t i = 1; i < s_pending_gap_count; i++) {
    const AudioCompanionSpoolPendingGap key = s_pending_gaps[i];
    int j = (int)i - 1;
    while (j >= 0 && s_pending_gaps[j].first_missing_sequence > key.first_missing_sequence) {
      s_pending_gaps[j + 1] = s_pending_gaps[j];
      j--;
    }
    s_pending_gaps[j + 1] = key;
  }
  for (uint8_t i = 0; (uint8_t)(i + 1) < s_pending_gap_count;) {
    AudioCompanionSpoolPendingGap *a = &s_pending_gaps[i];
    const AudioCompanionSpoolPendingGap *b = &s_pending_gaps[i + 1];
    if (a->reason != b->reason || prv_gap_end(a) != b->first_missing_sequence) {
      i++;
      continue;
    }
    a->missing_frame_count += b->missing_frame_count;
    prv_remove_gap(i + 1);
  }
}

//! Extend an existing same-reason record whose range abuts the new one, in either direction and
//! in any slot. Only the newest record used to be considered, but overflow, silence and pause
//! gaps interleave, so the record a new loss continues is frequently not the newest one.
static bool prv_try_exact_merge(uint32_t first_missing_sequence, uint32_t missing_frame_count,
                                uint64_t first_missing_sample_index, uint8_t reason) {
  for (uint8_t i = 0; i < s_pending_gap_count; i++) {
    AudioCompanionSpoolPendingGap *rec = &s_pending_gaps[i];
    if (rec->reason != reason) {
      continue;
    }
    if (prv_gap_end(rec) == first_missing_sequence) {
      rec->missing_frame_count += missing_frame_count;
      return true;
    }
    if ((first_missing_sequence + missing_frame_count) == rec->first_missing_sequence) {
      rec->first_missing_sequence = first_missing_sequence;
      rec->first_missing_sample_index = first_missing_sample_index;
      rec->missing_frame_count += missing_frame_count;
      return true;
    }
  }
  return false;
}

//! Last resort when every slot already holds a distinct, non-abutting range and another one has
//! to be stored: fold the sequence-neighbour pair that misrepresents the fewest frames, freeing a
//! slot so the incoming loss still gets its own truthful range.
//!
//! A fold costs the frames between the two ranges -- they are present, but the union claims them
//! missing, and the receiver only refills a claimed range from *re-delivered* frames, so an
//! over-claim it never sees again is permanent. When the reasons differ it also costs the frames
//! whose reason changes; charging that makes mixed folds the last ones picked. Genuine loss never
//! inherits SilenceSuppressed: the phone renders silence as calm "quiet" and hides it from the
//! coverage timeline, so that direction would erase real loss instead of over-reporting it.
static void prv_coalesce_cheapest_pair(void) {
  if (s_pending_gap_count < 2) {
    return;
  }
  uint8_t best = 0;
  uint64_t best_cost = UINT64_MAX;
  for (uint8_t i = 0; (uint8_t)(i + 1) < s_pending_gap_count; i++) {
    const AudioCompanionSpoolPendingGap *a = &s_pending_gaps[i];
    const AudioCompanionSpoolPendingGap *b = &s_pending_gaps[i + 1];
    const uint32_t end_a = prv_gap_end(a);
    uint64_t cost = (b->first_missing_sequence > end_a)
                        ? (uint64_t)(b->first_missing_sequence - end_a)
                        : 0;
    if (a->reason != b->reason) {
      const bool a_is_silence = (a->reason == AudioCompanionGapReasonSilenceSuppressed);
      cost += a_is_silence ? a->missing_frame_count : b->missing_frame_count;
    }
    if (cost < best_cost) {
      best_cost = cost;
      best = i;
    }
  }

  AudioCompanionSpoolPendingGap *a = &s_pending_gaps[best];
  const AudioCompanionSpoolPendingGap *b = &s_pending_gaps[best + 1];
  const uint32_t end_a = prv_gap_end(a);
  const uint32_t end_b = prv_gap_end(b);
  if (a->reason == AudioCompanionGapReasonSilenceSuppressed &&
      b->reason != AudioCompanionGapReasonSilenceSuppressed) {
    a->reason = b->reason;
  }
  a->missing_frame_count = ((end_b > end_a) ? end_b : end_a) - a->first_missing_sequence;
  if (best_cost > 0) {
    s_gap_coalesced_frames += (uint32_t)best_cost;
    PBL_LOG_WRN("Audio companion spool: gap slots full; folded %u frames into seq %u",
                (unsigned)best_cost, (unsigned)a->first_missing_sequence);
  }
  prv_remove_gap(best + 1);
}

static void prv_merge_gap(uint32_t first_missing_sequence, uint32_t missing_frame_count,
                          uint64_t first_missing_sample_index, uint8_t reason) {
  if (missing_frame_count == 0) {
    return;
  }
  if (prv_try_exact_merge(first_missing_sequence, missing_frame_count, first_missing_sample_index,
                          reason)) {
    prv_normalize_gaps();
    return;
  }
  if (s_pending_gap_count == MAX_PENDING_GAPS) {
    prv_coalesce_cheapest_pair();
  }
  s_pending_gaps[s_pending_gap_count++] = (AudioCompanionSpoolPendingGap){
    .valid = true,
    .first_missing_sequence = first_missing_sequence,
    .missing_frame_count = missing_frame_count,
    .first_missing_sample_index = first_missing_sample_index,
    .reason = reason,
  };
  s_gap_records++;
  prv_normalize_gaps();
}

//! Detach the oldest chunk, account its untrimmed frames as an overflow gap.
//! Returns the detached chunk (not freed) or NULL if there is none.
static SpoolChunk *prv_drop_oldest_chunk(void) {
  SpoolChunk *chunk = s_head;
  if (!chunk) {
    return NULL;
  }

  // Walk the records being lost so each gap covers exactly the span it describes. Sequence
  // numbers are not dense across a chunk: pauses and suppressed silence consume sequence space
  // without pushing frames, so one chunk can hold several disjoint runs. Describing them as a
  // single span would claim frames that were never in this chunk and miss the ones that were,
  // so emit one record per contiguous run.
  uint32_t dropped = 0;
  uint32_t run_first_seq = 0;
  uint64_t run_first_sample = 0;
  uint32_t run_frames = 0;
  for (uint16_t offset = chunk->trim_offset; offset < chunk->used;) {
    const SpoolRecordHeader *header = prv_record_at(chunk, offset);
    if (run_frames == 0) {
      run_first_seq = header->sequence;
      run_first_sample = header->sample_index;
    } else if (header->sequence != run_first_seq + run_frames) {
      prv_merge_gap(run_first_seq, run_frames, run_first_sample,
                    AudioCompanionGapReasonSpoolOverflow);
      run_first_seq = header->sequence;
      run_first_sample = header->sample_index;
      run_frames = 0;
    }
    run_frames++;
    dropped++;
    offset += prv_record_size(header);
  }
  if (run_frames > 0) {
    prv_merge_gap(run_first_seq, run_frames, run_first_sample,
                  AudioCompanionGapReasonSpoolOverflow);
  }

  if (dropped > 0) {
    s_dropped_overflow_frames += dropped;
    PBL_ASSERTN(s_frames_queued >= dropped);
    s_frames_queued -= dropped;
    // Frames the drain cursor had not reached yet are also gone.
    if (s_drain_chunk == chunk) {
      uint32_t unsent_dropped = 0;
      for (uint16_t offset = s_drain_offset; offset < chunk->used;) {
        const SpoolRecordHeader *header = prv_record_at(chunk, offset);
        unsent_dropped++;
        offset += prv_record_size(header);
      }
      PBL_ASSERTN(s_frames_unsent >= unsent_dropped);
      s_frames_unsent -= unsent_dropped;
    }
  }

  s_head = chunk->next;
  if (!s_head) {
    s_tail = NULL;
  }
  if (s_drain_chunk == chunk) {
    s_drain_chunk = s_head;
    s_drain_offset = s_head ? s_head->trim_offset : 0;
  }
  return chunk;
}

static void prv_append_chunk(SpoolChunk *chunk) {
  chunk->next = NULL;
  chunk->used = 0;
  chunk->trim_offset = 0;
  if (s_tail) {
    s_tail->next = chunk;
  } else {
    s_head = chunk;
  }
  s_tail = chunk;
  if (!s_drain_chunk) {
    s_drain_chunk = chunk;
    s_drain_offset = 0;
  }
}

static bool prv_release_oldest_chunk(void) {
  SpoolChunk *released = prv_drop_oldest_chunk();
  if (!released) {
    return false;
  }
  kernel_free(released);
  PBL_ASSERTN(s_current_bytes >= SPOOL_CHUNK_BYTES);
  s_current_bytes -= SPOOL_CHUNK_BYTES;
  return true;
}

static bool prv_has_optional_chunk(void) {
  return s_current_bytes > CONFIG_AUDIO_COMPANION_SPOOL_MIN_BYTES;
}

void audio_companion_spool_apply_pressure_policy(void) {
  while (prv_has_optional_chunk() &&
         prv_heap_free_bytes() < CONFIG_AUDIO_COMPANION_SPOOL_HEAP_RESERVE_BYTES) {
    if (!prv_release_oldest_chunk()) {
      break;
    }
  }
}

//! Get a chunk with room for a new record: grow if policy allows, otherwise
//! recycle the oldest chunk (recording an overflow gap).
static SpoolChunk *prv_get_writable_tail(size_t needed_bytes) {
  if (s_tail && (size_t)(SPOOL_CHUNK_DATA_BYTES - s_tail->used) >= needed_bytes) {
    return s_tail;
  }

  const bool below_ceiling =
      (s_current_bytes + SPOOL_CHUNK_BYTES) <= CONFIG_AUDIO_COMPANION_SPOOL_MAX_BYTES;
  const bool below_floor =
      (s_current_bytes + SPOOL_CHUNK_BYTES) <= CONFIG_AUDIO_COMPANION_SPOOL_MIN_BYTES;
  const bool heap_headroom =
      prv_heap_free_bytes() >=
      (CONFIG_AUDIO_COMPANION_SPOOL_HEAP_RESERVE_BYTES + SPOOL_CHUNK_BYTES);

  if (below_ceiling && (below_floor || heap_headroom)) {
    SpoolChunk *chunk = kernel_malloc(SPOOL_CHUNK_BYTES);
    if (chunk) {
      s_current_bytes += SPOOL_CHUNK_BYTES;
      if (s_current_bytes > s_high_water_bytes) {
        s_high_water_bytes = s_current_bytes;
      }
      prv_append_chunk(chunk);
      return chunk;
    }
  }

  // Cannot grow: recycle the oldest chunk in place (drop-oldest policy).
  SpoolChunk *recycled = prv_drop_oldest_chunk();
  if (!recycled) {
    return NULL;
  }
  prv_append_chunk(recycled);
  return recycled;
}

void audio_companion_spool_init(void) { audio_companion_spool_reset(); }

void audio_companion_spool_deinit(void) { audio_companion_spool_reset(); }

void audio_companion_spool_reset(void) {
  SpoolChunk *chunk = s_head;
  while (chunk) {
    SpoolChunk *next = chunk->next;
    kernel_free(chunk);
    chunk = next;
  }
  s_head = NULL;
  s_tail = NULL;
  s_drain_chunk = NULL;
  s_drain_offset = 0;
  memset(s_pending_gaps, 0, sizeof(s_pending_gaps));
  s_pending_gap_count = 0;
  s_pushed_frames = 0;
  s_dropped_overflow_frames = 0;
  s_gap_records = 0;
  s_gap_coalesced_frames = 0;
  s_frames_queued = 0;
  s_frames_unsent = 0;
  s_current_bytes = 0;
  s_high_water_bytes = 0;
}

bool audio_companion_spool_push(uint32_t sequence, uint64_t sample_index,
                                const uint8_t *payload, uint16_t length) {
  PBL_ASSERTN(payload != NULL);
  PBL_ASSERTN(length > 0 && length <= AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES);
  audio_companion_spool_apply_pressure_policy();

  const size_t needed = sizeof(SpoolRecordHeader) + length;
  SpoolChunk *chunk = prv_get_writable_tail(needed);
  if (!chunk) {
    prv_merge_gap(sequence, 1, sample_index, AudioCompanionGapReasonSpoolOverflow);
    s_dropped_overflow_frames++;
    return false;
  }

  SpoolRecordHeader header = {
    .sequence = sequence,
    .sample_index = sample_index,
    .length = length,
  };
  memcpy(&chunk->data[chunk->used], &header, sizeof(header));
  memcpy(&chunk->data[chunk->used + sizeof(header)], payload, length);
  chunk->used += needed;
  s_pushed_frames++;
  s_frames_queued++;
  s_frames_unsent++;
  return true;
}

bool audio_companion_spool_peek_batch(size_t max_message_bytes, size_t header_size,
                                      uint32_t *out_first_sequence,
                                      uint64_t *out_first_sample_index,
                                      uint8_t *out_frame_count, uint8_t *payload_buf,
                                      size_t payload_buf_size, size_t *out_payload_len) {
  if (!out_first_sequence || !out_first_sample_index || !out_frame_count || !payload_buf ||
      !out_payload_len) {
    return false;
  }

  const SpoolChunk *chunk = s_drain_chunk;
  uint16_t offset = s_drain_offset;
  size_t payload_len = 0;
  uint8_t frame_count = 0;

  while (chunk) {
    if (offset >= chunk->used) {
      chunk = chunk->next;
      offset = chunk ? chunk->trim_offset : 0;
      continue;
    }
    const SpoolRecordHeader *header = prv_record_at(chunk, offset);
    const size_t entry_bytes = sizeof(uint16_t) + header->length;
    if (frame_count == AUDIO_COMPANION_MAX_FRAMES_PER_DATA_MSG ||
        (header_size + payload_len + entry_bytes) > max_message_bytes ||
        (payload_len + entry_bytes) > payload_buf_size) {
      break;
    }
    if (frame_count == 0) {
      *out_first_sequence = header->sequence;
      *out_first_sample_index = header->sample_index;
    } else if (header->sequence != *out_first_sequence + frame_count) {
      break;  // batches must stay sequence-contiguous
    }
    memcpy(&payload_buf[payload_len], &header->length, sizeof(uint16_t));
    payload_len += sizeof(uint16_t);
    memcpy(&payload_buf[payload_len], &chunk->data[offset + sizeof(*header)], header->length);
    payload_len += header->length;
    frame_count++;
    offset += prv_record_size(header);
  }

  if (frame_count == 0) {
    return false;
  }
  *out_frame_count = frame_count;
  *out_payload_len = payload_len;
  return true;
}

void audio_companion_spool_mark_sent_through(uint32_t sequence) {
  while (s_drain_chunk) {
    if (s_drain_offset >= s_drain_chunk->used) {
      if (!s_drain_chunk->next) {
        break;
      }
      s_drain_chunk = s_drain_chunk->next;
      s_drain_offset = s_drain_chunk->trim_offset;
      continue;
    }
    const SpoolRecordHeader *header = prv_record_at(s_drain_chunk, s_drain_offset);
    if (header->sequence > sequence) {
      break;
    }
    s_drain_offset += prv_record_size(header);
    PBL_ASSERTN(s_frames_unsent > 0);
    s_frames_unsent--;
  }
}

void audio_companion_spool_trim_through(uint32_t sequence) {
  while (s_head) {
    SpoolChunk *chunk = s_head;
    if (chunk->trim_offset >= chunk->used) {
      if (chunk == s_tail) {
        // Fully consumed tail: reuse its space once the drain cursor is done too.
        if (s_drain_chunk == chunk && s_drain_offset >= chunk->used) {
          chunk->used = 0;
          chunk->trim_offset = 0;
          s_drain_offset = 0;
        }
        break;
      }
      s_head = chunk->next;
      if (s_drain_chunk == chunk) {
        s_drain_chunk = s_head;
        s_drain_offset = s_head->trim_offset;
      }
      kernel_free(chunk);
      s_current_bytes -= SPOOL_CHUNK_BYTES;
      continue;
    }
    const SpoolRecordHeader *header = prv_record_at(chunk, chunk->trim_offset);
    if (header->sequence > sequence) {
      break;
    }
    const uint16_t record_size = prv_record_size(header);
    // Keep the drain cursor at or ahead of the trim point.
    if (s_drain_chunk == chunk && s_drain_offset <= chunk->trim_offset) {
      s_drain_offset = chunk->trim_offset + record_size;
      PBL_ASSERTN(s_frames_unsent > 0);
      s_frames_unsent--;
    }
    chunk->trim_offset += record_size;
    PBL_ASSERTN(s_frames_queued > 0);
    s_frames_queued--;
  }
}

void audio_companion_spool_rewind_unsent(void) {
  s_drain_chunk = s_head;
  s_drain_offset = s_head ? s_head->trim_offset : 0;
  s_frames_unsent = s_frames_queued;
}

void audio_companion_spool_record_gap(uint32_t first_missing_sequence,
                                      uint32_t missing_frame_count,
                                      uint64_t first_missing_sample_index, uint8_t reason) {
  prv_merge_gap(first_missing_sequence, missing_frame_count, first_missing_sample_index,
                reason);
}

bool audio_companion_spool_take_pending_gap(AudioCompanionSpoolPendingGap *gap_out) {
  if (!gap_out || s_pending_gap_count == 0) {
    return false;
  }
  *gap_out = s_pending_gaps[0];
  if (s_pending_gap_count > 1) {
    memmove(&s_pending_gaps[0], &s_pending_gaps[1],
            (s_pending_gap_count - 1) * sizeof(s_pending_gaps[0]));
  }
  s_pending_gap_count--;
  memset(&s_pending_gaps[s_pending_gap_count], 0, sizeof(s_pending_gaps[0]));
  return true;
}

bool audio_companion_spool_has_pending_gap(void) { return s_pending_gap_count > 0; }

uint32_t audio_companion_spool_frames_pending_send(void) { return s_frames_unsent; }

uint32_t audio_companion_spool_heap_free_bytes(void) { return prv_heap_free_bytes(); }

void audio_companion_spool_get_stats(AudioCompanionSpoolStats *stats_out) {
  if (!stats_out) {
    return;
  }
  *stats_out = (AudioCompanionSpoolStats){
    .pushed_frames = s_pushed_frames,
    .dropped_overflow_frames = s_dropped_overflow_frames,
    .gap_records = s_gap_records,
    .gap_coalesced_frames = s_gap_coalesced_frames,
    .frames_queued = s_frames_queued,
    .frames_pending_send = s_frames_unsent,
    .current_bytes = s_current_bytes,
    .high_water_bytes = s_high_water_bytes,
  };
}
