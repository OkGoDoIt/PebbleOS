/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Bounded, heap-backed spool of encoded audio frames for the audio companion
//! service. Frames are stored packed (14-byte record header + variable payload)
//! in a FIFO of dynamically allocated chunks. The spool grows toward
//! CONFIG_AUDIO_COMPANION_SPOOL_MAX_BYTES while kernel heap headroom allows,
//! is always allowed to reach CONFIG_AUDIO_COMPANION_SPOOL_MIN_BYTES, and
//! drops the oldest frames (recording an explicit gap) when it cannot grow or
//! when heap pressure makes already-allocated optional chunks unsafe to keep.
//!
//! Two cursors: the *drain* cursor tracks the next frame to notify to the
//! receiver; the *trim* point tracks frames released by receiver checkpoints.
//! Frames between trim and drain have been sent but not yet checkpointed and
//! are kept for resend after a reconnect (audio_companion_spool_rewind_unsent).
//! Not thread-safe; callers hold the service lock.

typedef struct {
  bool valid;
  uint32_t first_missing_sequence;
  uint32_t missing_frame_count;
  uint64_t first_missing_sample_index;
  uint8_t reason;
} AudioCompanionSpoolPendingGap;

typedef struct {
  uint32_t pushed_frames;
  uint32_t dropped_overflow_frames;
  uint32_t gap_records;
  uint32_t frames_queued;       //!< frames currently held (trim point .. newest)
  uint32_t frames_pending_send; //!< frames at/after the drain cursor
  uint32_t current_bytes;       //!< currently allocated chunk bytes
  uint32_t high_water_bytes;
} AudioCompanionSpoolStats;

void audio_companion_spool_init(void);
//! Frees all chunks and clears all state.
void audio_companion_spool_deinit(void);
//! Clears frames, cursors, pending gap, and counters; keeps no memory.
void audio_companion_spool_reset(void);

//! Append one encoded frame. On overflow the oldest chunk is dropped and the
//! loss is merged into the pending gap record. Returns false only if the frame
//! could not be stored at all (allocation failure below the floor).
bool audio_companion_spool_push(uint32_t sequence, uint64_t sample_index,
                                const uint8_t *payload, uint16_t length);

//! Peek a batch of frames starting at the drain cursor. Frames are emitted as
//! length-prefixed entries into payload_buf, limited so that header_size plus
//! payload stays within max_message_bytes and frame count stays within
//! AUDIO_COMPANION_MAX_FRAMES_PER_DATA_MSG. Returns false when nothing to send.
bool audio_companion_spool_peek_batch(size_t max_message_bytes, size_t header_size,
                                      uint32_t *out_first_sequence,
                                      uint64_t *out_first_sample_index,
                                      uint8_t *out_frame_count, uint8_t *payload_buf,
                                      size_t payload_buf_size, size_t *out_payload_len);

//! Advance the drain cursor past all frames with sequence <= sequence.
void audio_companion_spool_mark_sent_through(uint32_t sequence);

//! Release frames with sequence <= sequence (receiver checkpoint). Never moves
//! past the drain cursor's last-sent frame... callers pass checkpoint values the
//! receiver derived from delivered data, which are always <= last sent.
void audio_companion_spool_trim_through(uint32_t sequence);

//! Reset the drain cursor back to the oldest retained frame (reconnect resend).
void audio_companion_spool_rewind_unsent(void);

void audio_companion_spool_record_gap(uint32_t first_missing_sequence,
                                      uint32_t missing_frame_count,
                                      uint64_t first_missing_sample_index, uint8_t reason);
bool audio_companion_spool_take_pending_gap(AudioCompanionSpoolPendingGap *gap_out);
bool audio_companion_spool_has_pending_gap(void);

//! Enforce the heap-reserve policy against already-allocated optional chunks.
//! This may drop the oldest retained audio down to the configured floor.
void audio_companion_spool_apply_pressure_policy(void);

uint32_t audio_companion_spool_frames_pending_send(void);
void audio_companion_spool_get_stats(AudioCompanionSpoolStats *stats_out);

#ifdef UNITTEST
//! Test seam: simulated free kernel-heap bytes used by the growth policy.
void audio_companion_spool_test_set_heap_free_bytes(uint32_t bytes);
#endif
