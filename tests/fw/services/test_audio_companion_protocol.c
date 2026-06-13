/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/audio_companion_private.h"

#include "audio_companion_fixtures.h"

#include <string.h>

// Golden-fixture coverage strategy: the firmware parses Info reads are not a thing — the
// firmware *builds* Info, control-out, and data messages, and *parses* control-in writes.
// So control_in fixtures exercise audio_companion_protocol_parse_control() (parse / reject /
// ignore), while info / control_out / data "parse" fixtures are byte-exact builder
// comparisons. Phone-side-only fixtures (info/data corruption + unknown data ids) are
// asserted to exist but skipped here; the KMP test suite owns them.

// Mirrors frame_payload() in pebble-audio-companion/tools/gen_fixtures.py.
static void prv_fill_frame_payload(uint32_t seq, uint16_t length, uint8_t *out) {
  uint32_t state = seq * 2654435761u;
  for (uint16_t i = 0; i < length;) {
    state = state * 1103515245u + 12345u;
    out[i++] = (state >> 16) & 0xFF;
  }
}

static const ProtocolFixture *prv_find_fixture(const char *name) {
  for (size_t i = 0; i < PROTOCOL_FIXTURE_COUNT; i++) {
    if (strcmp(s_protocol_fixtures[i].name, name) == 0) {
      return &s_protocol_fixtures[i];
    }
  }
  return NULL;
}

static void prv_assert_builder_matches(const char *name, const uint8_t *built, size_t built_len) {
  const ProtocolFixture *fx = prv_find_fixture(name);
  cl_assert_(fx != NULL, name);
  cl_assert_equal_i(built_len, fx->length);
  cl_assert_(memcmp(built, fx->data, fx->length) == 0, name);
}

static AudioCompanionParseResult prv_parse_fixture(const char *name,
                                                   AudioCompanionControlMsg *msg) {
  const ProtocolFixture *fx = prv_find_fixture(name);
  cl_assert_(fx != NULL, name);
  return audio_companion_protocol_parse_control(fx->data, fx->length, msg);
}

void test_audio_companion_protocol__initialize(void) {}
void test_audio_companion_protocol__cleanup(void) {}

// ---- Struct size lock-in ----

void test_audio_companion_protocol__struct_sizes(void) {
  cl_assert_equal_i(sizeof(AudioCompanionInfo), 20);
  cl_assert_equal_i(sizeof(AudioCompanionAuthRequestHeader), 36);
  cl_assert_equal_i(sizeof(AudioCompanionAuthRevokeMsg), 34);
  cl_assert_equal_i(sizeof(AudioCompanionCheckpointMsg), 26);
  cl_assert_equal_i(sizeof(AudioCompanionPauseRequestMsg), 3);
  cl_assert_equal_i(sizeof(AudioCompanionResumeRequestMsg), 2);
  cl_assert_equal_i(sizeof(AudioCompanionReceiverHealthMsg), 8);
  cl_assert_equal_i(sizeof(AudioCompanionAuthResultMsg), 4);
  cl_assert_equal_i(sizeof(AudioCompanionRevokedMsg), 2);
  cl_assert_equal_i(sizeof(AudioCompanionAckMsg), 3);
  cl_assert_equal_i(sizeof(AudioCompanionStateChangedMsg), 2);
  cl_assert_equal_i(sizeof(AudioCompanionErrorMsg), 6);
  cl_assert_equal_i(sizeof(AudioCompanionStreamStartMsg), 40);
  cl_assert_equal_i(sizeof(AudioCompanionStreamDataHeader), 20);
  cl_assert_equal_i(sizeof(AudioCompanionStreamGapMsg), 26);
  cl_assert_equal_i(sizeof(AudioCompanionStreamStopMsg), 22);
}

// ---- Control parse: nominal ----

void test_audio_companion_protocol__parse_auth_request(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("auth_request", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.msg_id, AudioCompanionCtrlMsgIdAuthRequest);
  cl_assert_equal_i(msg.auth_request.proto_version, 1);
  cl_assert_equal_i(msg.auth_request.request_token, 0x21);
  for (int i = 0; i < AUDIO_COMPANION_RECEIVER_ID_BYTES; i++) {
    cl_assert_equal_i(msg.auth_request.receiver_id[i], i);
  }
  cl_assert_equal_s(msg.auth_request.name, "Audio Companion");
}

void test_audio_companion_protocol__parse_auth_request_max_name(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("auth_request_max_name", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.auth_request.name_len, 24);
  cl_assert_equal_s(msg.auth_request.name, "xxxxxxxxxxxxxxxxxxxxxxxx");
}

void test_audio_companion_protocol__parse_auth_request_version_skew(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("auth_request_version_skew", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.auth_request.proto_version, 2);
}

void test_audio_companion_protocol__parse_auth_revoke(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("auth_revoke", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.auth_revoke.request_token, 0x26);
  for (int i = 0; i < AUDIO_COMPANION_RECEIVER_ID_BYTES; i++) {
    cl_assert_equal_i(msg.auth_revoke.receiver_id[i], i);
  }
}

void test_audio_companion_protocol__parse_checkpoint(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("checkpoint", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.checkpoint.request_token, 0x27);
  cl_assert(msg.checkpoint.stream_id == 0xA1B2C3D4u);
  cl_assert_equal_i(msg.checkpoint.highest_contiguous_sequence_persisted, 4999);
  cl_assert(msg.checkpoint.persisted_sample_index == 1600000ull);
  cl_assert_equal_i(msg.checkpoint.receiver_flags, 0);
  cl_assert_equal_i(msg.checkpoint.free_storage_hint_kb, 870400);
}

void test_audio_companion_protocol__parse_checkpoint_boundary(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("checkpoint_boundary", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.checkpoint.request_token, 0xFF);
  cl_assert(msg.checkpoint.stream_id == 0xFFFFFFFFu);
  cl_assert(msg.checkpoint.highest_contiguous_sequence_persisted == 0xFFFFFFFFu);
  cl_assert(msg.checkpoint.persisted_sample_index == 0xFFFFFFFFFFFFFFFFull);
  cl_assert_equal_i(msg.checkpoint.receiver_flags,
                    AUDIO_COMPANION_RECEIVER_FLAG_LOW_STORAGE |
                        AUDIO_COMPANION_RECEIVER_FLAG_PAUSE_REQUESTED);
  cl_assert_equal_i(msg.checkpoint.free_storage_hint_kb, 0);
}

void test_audio_companion_protocol__parse_checkpoint_appended_fields(void) {
  // Future-version message with appended fields must parse (append-only versioning).
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("checkpoint_v2_appended", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.checkpoint.request_token, 0x28);
  cl_assert_equal_i(msg.checkpoint.stream_id, 7);
  cl_assert_equal_i(msg.checkpoint.highest_contiguous_sequence_persisted, 10);
  cl_assert(msg.checkpoint.persisted_sample_index == 3200ull);
  cl_assert_equal_i(msg.checkpoint.free_storage_hint_kb, 1024);
}

void test_audio_companion_protocol__parse_pause_resume_health(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("pause_request", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.pause_request.request_token, 0x2A);
  cl_assert_equal_i(msg.pause_request.reason, 1);

  cl_assert_equal_i(prv_parse_fixture("resume_request", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.resume_request.request_token, 0x2B);

  cl_assert_equal_i(prv_parse_fixture("receiver_health", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.receiver_health.request_token, 0x2C);
  cl_assert_equal_i(msg.receiver_health.battery_pct, 76);
  cl_assert_equal_i(msg.receiver_health.app_state, 2);
  cl_assert_equal_i(msg.receiver_health.queue_depth_frames, 130);
}

// ---- Control parse: malformed + unknown (table-driven from fixtures) ----

void test_audio_companion_protocol__parse_rejects_and_ignores(void) {
  AudioCompanionControlMsg msg;
  for (size_t i = 0; i < PROTOCOL_FIXTURE_COUNT; i++) {
    const ProtocolFixture *fx = &s_protocol_fixtures[i];
    if (fx->channel != FixtureChannelControlIn) {
      continue;
    }
    const AudioCompanionParseResult result =
        audio_companion_protocol_parse_control(fx->data, fx->length, &msg);
    switch (fx->expect) {
      case FixtureExpectParse:
        cl_assert_(result == AudioCompanionParseResultOk, fx->name);
        break;
      case FixtureExpectReject:
        cl_assert_(result == AudioCompanionParseResultMalformed, fx->name);
        break;
      case FixtureExpectIgnore:
        cl_assert_(result == AudioCompanionParseResultUnknown, fx->name);
        break;
    }
  }
}

// ---- Builders: byte-exact against golden fixtures ----

void test_audio_companion_protocol__build_info(void) {
  uint8_t buf[64];
  const AudioCompanionInfo streaming = {
    .info_version = 1,
    .protocol_min = 1,
    .protocol_max = 1,
    .service_state = AudioCompanionServiceStateStreaming,
    .codec_bitmap = 0x01,
    .flags = AUDIO_COMPANION_INFO_FLAG_RECEIVER_BOUND | AUDIO_COMPANION_INFO_FLAG_ENABLED,
    .fw_version_packed = (4u << 24) | (9u << 16) | 2u,
  };
  size_t len = audio_companion_protocol_build_info(buf, sizeof(buf), &streaming);
  prv_assert_builder_matches("info_streaming", buf, len);

  const AudioCompanionInfo disabled = {
    .info_version = 1,
    .protocol_min = 1,
    .protocol_max = 1,
    .service_state = AudioCompanionServiceStateDisabled,
    .codec_bitmap = 0x01,
    .flags = 0,
    .fw_version_packed = (4u << 24) | (9u << 16) | 2u,
  };
  len = audio_companion_protocol_build_info(buf, sizeof(buf), &disabled);
  prv_assert_builder_matches("info_disabled", buf, len);
}

void test_audio_companion_protocol__build_control_out(void) {
  uint8_t buf[16];
  size_t len;

  len = audio_companion_protocol_build_auth_result(buf, sizeof(buf), 0x21,
                                                   AudioCompanionAuthStatusOk, 1);
  prv_assert_builder_matches("auth_result_ok", buf, len);

  len = audio_companion_protocol_build_auth_result(
      buf, sizeof(buf), 0x21, AudioCompanionAuthStatusPendingUserConsent, 0);
  prv_assert_builder_matches("auth_result_pending", buf, len);

  len = audio_companion_protocol_build_auth_result(buf, sizeof(buf), 0x21,
                                                   AudioCompanionAuthStatusDeniedMismatch, 0);
  prv_assert_builder_matches("auth_result_denied_mismatch", buf, len);

  len = audio_companion_protocol_build_revoked(buf, sizeof(buf),
                                               AudioCompanionRevokedReasonUserOnWatch);
  prv_assert_builder_matches("revoked_user", buf, len);

  len = audio_companion_protocol_build_ack(buf, sizeof(buf), 0x27, AudioCompanionAckStatusOk);
  prv_assert_builder_matches("ack_ok", buf, len);

  len = audio_companion_protocol_build_state_changed(buf, sizeof(buf),
                                                     AudioCompanionServiceStateStreaming);
  prv_assert_builder_matches("state_changed_streaming", buf, len);

  len = audio_companion_protocol_build_state_changed(buf, sizeof(buf),
                                                     AudioCompanionServiceStatePausedPowerSave);
  prv_assert_builder_matches("state_changed_power_save", buf, len);

  len = audio_companion_protocol_build_error(buf, sizeof(buf),
                                             AudioCompanionErrorCodeMalformed, 0);
  prv_assert_builder_matches("error_malformed", buf, len);
}

void test_audio_companion_protocol__build_stream_start(void) {
  uint8_t buf[64];
  const AudioCompanionStreamStartMsg params = {
    .protocol_version = 1,
    .stream_id = 0x5EED0001,
    .codec_id = AudioCompanionCodecSpeexWideband,
    .channels = 1,
    .frame_samples = AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES,
    .sample_rate_hz = AUDIO_COMPANION_DEFAULT_SAMPLE_RATE_HZ,
    .bit_rate_bps = AUDIO_COMPANION_DEFAULT_BIT_RATE_BPS,
    .frame_duration_ms = AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
    .start_time_ms = 1781000000000ull,
    .start_monotonic_ms = 86400123ull,
    .flags = 0,
  };
  const size_t len = audio_companion_protocol_build_stream_start(buf, sizeof(buf), &params);
  prv_assert_builder_matches("stream_start", buf, len);
}

static size_t prv_build_data_msg(uint8_t *buf, size_t buf_size, uint32_t stream_id,
                                 uint32_t first_sequence, uint64_t first_sample_index,
                                 uint8_t frame_count, const uint16_t *frame_lengths) {
  size_t len = audio_companion_protocol_build_stream_data_header(
      buf, buf_size, stream_id, first_sequence, first_sample_index, frame_count, 0);
  cl_assert(len > 0);
  for (uint8_t i = 0; i < frame_count; i++) {
    const uint16_t frame_len = frame_lengths[i];
    cl_assert(len + sizeof(uint16_t) + frame_len <= buf_size);
    memcpy(buf + len, &frame_len, sizeof(frame_len));
    len += sizeof(frame_len);
    prv_fill_frame_payload(first_sequence + i, frame_len, buf + len);
    len += frame_len;
  }
  return len;
}

void test_audio_companion_protocol__build_stream_data(void) {
  uint8_t buf[1024];
  uint16_t lengths[32];
  size_t len;

  lengths[0] = 25;
  len = prv_build_data_msg(buf, sizeof(buf), 0x5EED0001, 0, 0, 1, lengths);
  prv_assert_builder_matches("stream_data_1frame", buf, len);

  for (int i = 0; i < 8; i++) {
    lengths[i] = 22 + i;
  }
  len = prv_build_data_msg(buf, sizeof(buf), 0x5EED0001, 800, 256000ull, 8, lengths);
  prv_assert_builder_matches("stream_data_8frames", buf, len);

  for (int i = 0; i < 32; i++) {
    lengths[i] = 4;
  }
  len = prv_build_data_msg(buf, sizeof(buf), 0x5EED0001, 1000, 320000ull, 32, lengths);
  prv_assert_builder_matches("stream_data_32frames", buf, len);

  lengths[0] = AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES;
  len = prv_build_data_msg(buf, sizeof(buf), 0x5EED0001, 42, 13440ull, 1, lengths);
  prv_assert_builder_matches("stream_data_max_frame_bytes", buf, len);
}

void test_audio_companion_protocol__build_stream_data_header_rejects_bad_count(void) {
  uint8_t buf[64];
  cl_assert_equal_i(
      audio_companion_protocol_build_stream_data_header(buf, sizeof(buf), 1, 0, 0, 0, 0), 0);
  cl_assert_equal_i(audio_companion_protocol_build_stream_data_header(
                        buf, sizeof(buf), 1, 0, 0,
                        AUDIO_COMPANION_MAX_FRAMES_PER_DATA_MSG + 1, 0),
                    0);
}

void test_audio_companion_protocol__build_stream_gap(void) {
  static const struct {
    const char *name;
    uint8_t reason;
  } cases[] = {
    { "stream_gap_spool_overflow", AudioCompanionGapReasonSpoolOverflow },
    { "stream_gap_mic_conflict", AudioCompanionGapReasonMicConflict },
    { "stream_gap_user_disabled", AudioCompanionGapReasonUserDisabled },
    { "stream_gap_low_battery", AudioCompanionGapReasonLowBattery },
    { "stream_gap_codec_error", AudioCompanionGapReasonCodecError },
    { "stream_gap_transport_reset", AudioCompanionGapReasonTransportReset },
    { "stream_gap_power_save", AudioCompanionGapReasonPowerSave },
  };
  uint8_t buf[64];
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const AudioCompanionStreamGapMsg params = {
      .stream_id = 0x5EED0001,
      .first_missing_sequence = 5000 + cases[i].reason,
      .missing_frame_count = 250,
      .first_missing_sample_index = 1600000ull + (uint64_t)cases[i].reason * 320,
      .reason = cases[i].reason,
      .watch_drop_counter = 250,
    };
    const size_t len = audio_companion_protocol_build_stream_gap(buf, sizeof(buf), &params);
    prv_assert_builder_matches(cases[i].name, buf, len);
  }

  const AudioCompanionStreamGapMsg unknown_count = {
    .stream_id = 0x5EED0001,
    .first_missing_sequence = 7000,
    .missing_frame_count = 0,
    .first_missing_sample_index = 2240000ull,
    .reason = AudioCompanionGapReasonMicConflict,
    .watch_drop_counter = 250,
  };
  const size_t len = audio_companion_protocol_build_stream_gap(buf, sizeof(buf), &unknown_count);
  prv_assert_builder_matches("stream_gap_unknown_count", buf, len);
}

void test_audio_companion_protocol__build_stream_stop(void) {
  uint8_t buf[64];
  const AudioCompanionStreamStopMsg user_disabled = {
    .stream_id = 0x5EED0001,
    .reason = AudioCompanionStopReasonUserDisabled,
    .final_sequence = 9999,
    .final_sample_index = 3200000ull,
    .counters_crc_or_zero = 0,
  };
  size_t len = audio_companion_protocol_build_stream_stop(buf, sizeof(buf), &user_disabled);
  prv_assert_builder_matches("stream_stop_user_disabled", buf, len);

  const AudioCompanionStreamStopMsg shutdown = {
    .stream_id = 0x5EED0001,
    .reason = AudioCompanionStopReasonShutdown,
    .final_sequence = 12000,
    .final_sample_index = 3840000ull,
    .counters_crc_or_zero = 0,
  };
  len = audio_companion_protocol_build_stream_stop(buf, sizeof(buf), &shutdown);
  prv_assert_builder_matches("stream_stop_shutdown", buf, len);
}

// ---- Builders: undersized buffers fail closed ----

void test_audio_companion_protocol__builders_reject_small_buffers(void) {
  uint8_t buf[4];
  const AudioCompanionInfo info = { .info_version = 1 };
  cl_assert_equal_i(audio_companion_protocol_build_info(buf, 4, &info), 0);
  cl_assert_equal_i(audio_companion_protocol_build_auth_result(buf, 3, 0, 0, 0), 0);
  const AudioCompanionStreamStartMsg start = { .stream_id = 1 };
  cl_assert_equal_i(audio_companion_protocol_build_stream_start(buf, 4, &start), 0);
}
