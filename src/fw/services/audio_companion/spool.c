/* SPDX-License-Identifier: Apache-2.0 */

#include "spool.h"

#include "pbl/services/audio_companion_private.h"

#include "kernel/pbl_malloc.h"
#include "system/passert.h"
#include "util/attributes.h"

#ifndef UNITTEST
#include "kernel/kernel_heap.h"
#include "util/heap.h"
#endif

#include <string.h>

#ifndef CONFIG_AUDIO_COMPANION_SPOOL_MIN_BYTES
#define CONFIG_AUDIO_COMPANION_SPOOL_MIN_BYTES 8192
#endif
#ifndef CONFIG_AUDIO_COMPANION_SPOOL_MAX_BYTES
#define CONFIG_AUDIO_COMPANION_SPOOL_MAX_BYTES 65536
#endif
#ifndef CONFIG_AUDIO_COMPANION_SPOOL_HEAP_RESERVE_BYTES
#define CONFIG_AUDIO_COMPANION_SPOOL_HEAP_RESERVE_BYTES 32768
#endif

#define SPOOL_CHUNK_BYTES (4096)

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
static AudioCompanionSpoolPendingGap s_pending_gap;
static uint32_t s_pushed_frames;
static uint32_t s_dropped_overflow_frames;
static uint32_t s_gap_records;
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

static void prv_merge_gap(uint32_t first_missing_sequence, uint32_t missing_frame_count,
                          uint64_t first_missing_sample_index, uint8_t reason) {
  if (!s_pending_gap.valid) {
    s_pending_gap = (AudioCompanionSpoolPendingGap){
      .valid = true,
      .first_missing_sequence = first_missing_sequence,
      .missing_frame_count = missing_frame_count,
      .first_missing_sample_index = first_missing_sample_index,
      .reason = reason,
    };
    s_gap_records++;
  } else {
    s_pending_gap.missing_frame_count += missing_frame_count;
  }
}

//! Detach the oldest chunk, account its untrimmed frames as an overflow gap.
//! Returns the detached chunk (not freed) or NULL if there is none.
static SpoolChunk *prv_drop_oldest_chunk(void) {
  SpoolChunk *chunk = s_head;
  if (!chunk) {
    return NULL;
  }

  // Walk the records being lost so the gap covers them precisely.
  uint32_t dropped = 0;
  uint32_t first_seq = 0;
  uint64_t first_sample = 0;
  for (uint16_t offset = chunk->trim_offset; offset < chunk->used;) {
    const SpoolRecordHeader *header = prv_record_at(chunk, offset);
    if (dropped == 0) {
      first_seq = header->sequence;
      first_sample = header->sample_index;
    }
    dropped++;
    offset += prv_record_size(header);
  }

  if (dropped > 0) {
    prv_merge_gap(first_seq, dropped, first_sample, AudioCompanionGapReasonSpoolOverflow);
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
  memset(&s_pending_gap, 0, sizeof(s_pending_gap));
  s_pushed_frames = 0;
  s_dropped_overflow_frames = 0;
  s_gap_records = 0;
  s_frames_queued = 0;
  s_frames_unsent = 0;
  s_current_bytes = 0;
  s_high_water_bytes = 0;
}

bool audio_companion_spool_push(uint32_t sequence, uint64_t sample_index,
                                const uint8_t *payload, uint16_t length) {
  PBL_ASSERTN(payload != NULL);
  PBL_ASSERTN(length > 0 && length <= AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES);

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
  if (!gap_out || !s_pending_gap.valid) {
    return false;
  }
  *gap_out = s_pending_gap;
  memset(&s_pending_gap, 0, sizeof(s_pending_gap));
  return true;
}

bool audio_companion_spool_has_pending_gap(void) { return s_pending_gap.valid; }

uint32_t audio_companion_spool_frames_pending_send(void) { return s_frames_unsent; }

void audio_companion_spool_get_stats(AudioCompanionSpoolStats *stats_out) {
  if (!stats_out) {
    return;
  }
  *stats_out = (AudioCompanionSpoolStats){
    .pushed_frames = s_pushed_frames,
    .dropped_overflow_frames = s_dropped_overflow_frames,
    .gap_records = s_gap_records,
    .frames_queued = s_frames_queued,
    .frames_pending_send = s_frames_unsent,
    .current_bytes = s_current_bytes,
    .high_water_bytes = s_high_water_bytes,
  };
}
