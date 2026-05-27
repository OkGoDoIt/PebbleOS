/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  uint32_t sequence;
  uint64_t sample_index;
  uint16_t encoded_length;
  uint16_t flags;
  uint8_t payload[200];
} BackgroundAudioSpoolFrame;

typedef struct {
  bool valid;
  uint32_t first_missing_sequence;
  uint32_t missing_frame_count;
  uint64_t first_missing_sample_index;
  uint8_t reason;
} BackgroundAudioSpoolPendingGap;

void background_audio_spool_init(void);
void background_audio_spool_reset(void);

bool background_audio_spool_push(uint32_t sequence, uint64_t sample_index,
                               const uint8_t *payload, uint16_t encoded_length,
                               uint16_t flags);

uint32_t background_audio_spool_depth(void);

bool background_audio_spool_peek_batch(uint32_t max_payload_bytes,
                                       uint32_t *out_first_sequence,
                                       uint64_t *out_first_sample_index,
                                       uint8_t *out_frame_count,
                                       uint8_t *payload_buf, size_t payload_buf_size,
                                       size_t *out_payload_len);

void background_audio_spool_pop_through(uint32_t sequence);

bool background_audio_spool_take_pending_gap(BackgroundAudioSpoolPendingGap *out);

void background_audio_spool_record_gap(uint32_t first_missing_sequence,
                                       uint32_t missing_frame_count,
                                       uint64_t first_missing_sample_index,
                                       uint8_t reason);

bool background_audio_spool_has_pending_gap(void);

uint32_t background_audio_spool_dropped_overflow_count(void);
