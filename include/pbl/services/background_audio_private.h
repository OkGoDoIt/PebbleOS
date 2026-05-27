/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/attributes.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

//! Private audio endpoint command ids for continuous background streaming.
typedef enum {
  BackgroundAudioMsgIdStreamStart = 0x10,
  BackgroundAudioMsgIdStreamData = 0x11,
  BackgroundAudioMsgIdStreamGap = 0x12,
  BackgroundAudioMsgIdStreamCheckpoint = 0x13,
  BackgroundAudioMsgIdStreamStop = 0x14,
  BackgroundAudioMsgIdStreamControl = 0x15,
} BackgroundAudioMsgId;

typedef enum {
  BackgroundAudioCodecSpeexWideband = 0x01,
  BackgroundAudioCodecPcm16Debug = 0x02,
  BackgroundAudioCodecOpusReserved = 0x03,
  BackgroundAudioCodecLc3Reserved = 0x04,
} BackgroundAudioCodecId;

typedef enum {
  BackgroundAudioGapReasonSpoolOverflow = 0x01,
  BackgroundAudioGapReasonMicConflict = 0x02,
  BackgroundAudioGapReasonUserDisabled = 0x03,
  BackgroundAudioGapReasonLowBattery = 0x04,
  BackgroundAudioGapReasonCodecError = 0x05,
  BackgroundAudioGapReasonTransportReset = 0x06,
} BackgroundAudioGapReason;

typedef enum {
  BackgroundAudioStopReasonUserDisabled = 0x01,
  BackgroundAudioStopReasonPolicy = 0x02,
  BackgroundAudioStopReasonError = 0x03,
  BackgroundAudioStopReasonShutdown = 0x04,
} BackgroundAudioStopReason;

#define BACKGROUND_AUDIO_RECEIVER_FLAG_LOW_STORAGE (1u << 0)
#define BACKGROUND_AUDIO_RECEIVER_FLAG_PAUSE_REQUESTED (1u << 1)

#define BACKGROUND_AUDIO_PROTOCOL_VERSION (1)
#define BACKGROUND_AUDIO_MAX_ENCODED_FRAME_BYTES (200)
#define BACKGROUND_AUDIO_DEFAULT_FRAME_SAMPLES (320)
#define BACKGROUND_AUDIO_DEFAULT_SAMPLE_RATE_HZ (16000)
#define BACKGROUND_AUDIO_DEFAULT_FRAME_DURATION_MS (20)
#define BACKGROUND_AUDIO_DEFAULT_BIT_RATE_BPS (9800)

typedef struct PACKED {
  uint8_t command_id;
  uint8_t protocol_version;
  uint32_t stream_id;
  uint8_t codec_id;
  uint8_t channels;
  uint16_t frame_samples;
  uint32_t sample_rate_hz;
  uint32_t bit_rate_bps;
  uint16_t frame_duration_ms;
  uint64_t start_time_ms;
  uint64_t start_monotonic_ms;
  uint32_t flags;
} BackgroundAudioStreamStartMsg;

typedef struct PACKED {
  uint8_t command_id;
  uint32_t stream_id;
  uint32_t first_sequence;
  uint64_t first_sample_index;
  uint8_t frame_count;
  uint16_t flags;
  //! Followed by frame_count * (uint16_t length + payload[length])
} BackgroundAudioStreamDataHeader;

typedef struct PACKED {
  uint8_t command_id;
  uint32_t stream_id;
  uint32_t first_missing_sequence;
  uint32_t missing_frame_count;
  uint64_t first_missing_sample_index;
  uint8_t reason;
  uint32_t watch_drop_counter;
} BackgroundAudioStreamGapMsg;

typedef struct PACKED {
  uint8_t command_id;
  uint32_t stream_id;
  uint32_t highest_contiguous_sequence_persisted;
  uint64_t persisted_sample_index;
  uint32_t receiver_flags;
  uint32_t free_storage_hint_kb;
} BackgroundAudioStreamCheckpointMsg;

typedef struct PACKED {
  uint8_t command_id;
  uint32_t stream_id;
  uint8_t reason;
  uint32_t final_sequence;
  uint64_t final_sample_index;
  uint32_t counters_crc_or_zero;
} BackgroundAudioStreamStopMsg;

size_t background_audio_stream_start_msg_size(void);
size_t background_audio_stream_data_header_size(void);
size_t background_audio_stream_gap_msg_size(void);
size_t background_audio_stream_stop_msg_size(void);

bool background_audio_parse_checkpoint_msg(const uint8_t *data, size_t size,
                                           BackgroundAudioStreamCheckpointMsg *out);
