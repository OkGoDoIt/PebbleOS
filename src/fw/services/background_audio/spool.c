/* SPDX-License-Identifier: Apache-2.0 */

#include "spool.h"

#include "pbl/services/background_audio_private.h"

#include "system/passert.h"

#include <string.h>

#ifndef CONFIG_BACKGROUND_AUDIO_SPOOL_CAPACITY
#define CONFIG_BACKGROUND_AUDIO_SPOOL_CAPACITY 512
#endif

static BackgroundAudioSpoolFrame s_frames[CONFIG_BACKGROUND_AUDIO_SPOOL_CAPACITY];
static uint32_t s_head;
static uint32_t s_count;
static uint32_t s_dropped_overflow;
static BackgroundAudioSpoolPendingGap s_pending_gap;

void background_audio_spool_init(void) {
  background_audio_spool_reset();
}

void background_audio_spool_reset(void) {
  s_head = 0;
  s_count = 0;
  s_dropped_overflow = 0;
  memset(&s_pending_gap, 0, sizeof(s_pending_gap));
}

static uint32_t prv_index(uint32_t offset) {
  return (s_head + offset) % CONFIG_BACKGROUND_AUDIO_SPOOL_CAPACITY;
}

bool background_audio_spool_push(uint32_t sequence, uint64_t sample_index,
                                 const uint8_t *payload, uint16_t encoded_length,
                                 uint16_t flags) {
  PBL_ASSERTN(payload != NULL);
  PBL_ASSERTN(encoded_length <= BACKGROUND_AUDIO_MAX_ENCODED_FRAME_BYTES);

  if (s_count == CONFIG_BACKGROUND_AUDIO_SPOOL_CAPACITY) {
    BackgroundAudioSpoolFrame *oldest = &s_frames[s_head];
    if (!s_pending_gap.valid) {
      s_pending_gap = (BackgroundAudioSpoolPendingGap) {
        .valid = true,
        .first_missing_sequence = oldest->sequence,
        .missing_frame_count = 1,
        .first_missing_sample_index = oldest->sample_index,
        .reason = BackgroundAudioGapReasonSpoolOverflow,
      };
    } else {
      s_pending_gap.missing_frame_count++;
    }
    s_head = prv_index(1);
    s_count--;
    s_dropped_overflow++;
  }

  uint32_t tail_index = prv_index(s_count);
  BackgroundAudioSpoolFrame *frame = &s_frames[tail_index];
  *frame = (BackgroundAudioSpoolFrame) {
    .sequence = sequence,
    .sample_index = sample_index,
    .encoded_length = encoded_length,
    .flags = flags,
  };
  memcpy(frame->payload, payload, encoded_length);
  s_count++;
  return true;
}

uint32_t background_audio_spool_depth(void) {
  return s_count;
}

bool background_audio_spool_peek_batch(uint32_t max_payload_bytes,
                                       uint32_t *out_first_sequence,
                                       uint64_t *out_first_sample_index,
                                       uint8_t *out_frame_count,
                                       uint8_t *payload_buf, size_t payload_buf_size,
                                       size_t *out_payload_len) {
  if (s_count == 0 || !out_first_sequence || !out_first_sample_index ||
      !out_frame_count || !payload_buf || !out_payload_len) {
    return false;
  }

  size_t payload_len = 0;
  uint8_t frame_count = 0;
  const size_t header_size = background_audio_stream_data_header_size();

  for (uint32_t i = 0; i < s_count; i++) {
    const BackgroundAudioSpoolFrame *frame = &s_frames[prv_index(i)];
    const size_t frame_bytes = sizeof(uint16_t) + frame->encoded_length;
    const size_t total_if_added = header_size + payload_len + frame_bytes;
    if (total_if_added > max_payload_bytes || (payload_len + frame_bytes) > payload_buf_size) {
      break;
    }
    if (frame_count == 0) {
      *out_first_sequence = frame->sequence;
      *out_first_sample_index = frame->sample_index;
    }
    uint16_t encoded_length = frame->encoded_length;
    memcpy(payload_buf + payload_len, &encoded_length, sizeof(encoded_length));
    payload_len += sizeof(encoded_length);
    memcpy(payload_buf + payload_len, frame->payload, frame->encoded_length);
    payload_len += frame->encoded_length;
    frame_count++;
  }

  if (frame_count == 0) {
    return false;
  }

  *out_frame_count = frame_count;
  *out_payload_len = payload_len;
  return true;
}

void background_audio_spool_pop_through(uint32_t sequence) {
  while (s_count > 0) {
    const BackgroundAudioSpoolFrame *frame = &s_frames[s_head];
    if (frame->sequence > sequence) {
      break;
    }
    s_head = prv_index(1);
    s_count--;
    if (frame->sequence == sequence) {
      break;
    }
  }
}

bool background_audio_spool_take_pending_gap(BackgroundAudioSpoolPendingGap *out) {
  if (!out || !s_pending_gap.valid) {
    return false;
  }
  *out = s_pending_gap;
  memset(&s_pending_gap, 0, sizeof(s_pending_gap));
  return true;
}

void background_audio_spool_record_gap(uint32_t first_missing_sequence,
                                       uint32_t missing_frame_count,
                                       uint64_t first_missing_sample_index,
                                       uint8_t reason) {
  if (!s_pending_gap.valid) {
    s_pending_gap = (BackgroundAudioSpoolPendingGap) {
      .valid = true,
      .first_missing_sequence = first_missing_sequence,
      .missing_frame_count = missing_frame_count,
      .first_missing_sample_index = first_missing_sample_index,
      .reason = reason,
    };
  } else {
    s_pending_gap.missing_frame_count += missing_frame_count;
  }
}

bool background_audio_spool_has_pending_gap(void) {
  return s_pending_gap.valid;
}

uint32_t background_audio_spool_dropped_overflow_count(void) {
  return s_dropped_overflow;
}
