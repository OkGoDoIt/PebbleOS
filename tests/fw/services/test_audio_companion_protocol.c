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
  cl_assert_equal_i(sizeof(AudioCompanionInfo), AUDIO_COMPANION_INFO_SIZE);
  cl_assert_equal_i(AUDIO_COMPANION_INFO_V1_SIZE, 20);
  cl_assert_equal_i(AUDIO_COMPANION_INFO_SIZE, 32);
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

  cl_assert_equal_i(prv_parse_fixture("enable_request", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.enable_request.request_token, 0x2D);

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

  // Version 2 carries the last restart: a watchdog reset, then the guard stood down. The phone
  // reads this instead of an unexplained "disabled".
  const AudioCompanionInfo after_fault = {
    .info_version = 2,
    .protocol_min = 1,
    .protocol_max = 1,
    .service_state = AudioCompanionServiceStateDisabled,
    .codec_bitmap = 0x01,
    .flags = AUDIO_COMPANION_INFO_FLAG_RECEIVER_BOUND | AUDIO_COMPANION_INFO_FLAG_BACKPRESSURE_COUNTER,
    .fw_version_packed = (4u << 24) | (36u << 16) | 0u,
    .send_backpressure_events = 7,
    .last_reboot_reason = 16,  // RebootReasonCode_Watchdog
    .fault_boots = 0,
    .error_reboots = 5,
    .stood_down = 1,
    .fault_pc = 0x1000ABCDu,
    .fault_lr = 0x1000EF01u,
  };
  len = audio_companion_protocol_build_info(buf, sizeof(buf), &after_fault);
  cl_assert_equal_i(len, AUDIO_COMPANION_INFO_SIZE);
  prv_assert_builder_matches("info_v2_after_fault", buf, len);
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

  len = audio_companion_protocol_build_phone_media_query(buf, sizeof(buf));
  prv_assert_builder_matches("phone_media_query", buf, len);
}

void test_audio_companion_protocol__parse_phone_media_state(void) {
  const ProtocolFixture *fixture = prv_find_fixture("phone_media_state_playing");
  cl_assert(fixture);
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(audio_companion_protocol_parse_control(fixture->data, fixture->length, &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.msg_id, AudioCompanionCtrlMsgIdPhoneMediaState);
  cl_assert_equal_i(msg.phone_media_state.request_token, 0x2E);
  cl_assert_equal_i(msg.phone_media_state.flags, AUDIO_COMPANION_PHONE_MEDIA_CHECKED |
                                                     AUDIO_COMPANION_PHONE_MEDIA_OTHER_AUDIO |
                                                     AUDIO_COMPANION_PHONE_MEDIA_PRIMARY_AUDIO);
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
    { "stream_gap_silence_suppressed", AudioCompanionGapReasonSilenceSuppressed },
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

// ---- Muse gadget setup relay ----

void test_audio_companion_protocol__parse_muse_setup_requests(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("muse_setup_open", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.msg_id, AudioCompanionCtrlMsgIdMuseSetupOpen);
  cl_assert_equal_i(msg.muse_setup_open.request_token, 0x31);
  cl_assert_equal_i(msg.muse_setup_open.window_secs, 300);
  cl_assert(memcmp(msg.muse_setup_open.suffix, "F1A69C", AUDIO_COMPANION_MUSE_SUFFIX_LEN) == 0);
  cl_assert_equal_i(msg.muse_setup_open.flags, 0);

  cl_assert_equal_i(prv_parse_fixture("muse_setup_close_paired", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.msg_id, AudioCompanionCtrlMsgIdMuseSetupClose);
  cl_assert_equal_i(msg.muse_setup_close.request_token, 0x33);
  cl_assert_equal_i(msg.muse_setup_close.reason, AudioCompanionMuseAppCloseReasonPaired);

  cl_assert_equal_i(prv_parse_fixture("muse_setup_tx_device_info", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.msg_id, AudioCompanionCtrlMsgIdMuseSetupTx);
  cl_assert_equal_i(msg.muse_setup_tx.request_token, 0x35);
  const ProtocolFixture *fx = prv_find_fixture("muse_setup_tx_device_info");
  cl_assert_equal_i(msg.muse_setup_tx.length, fx->length - 3);
  cl_assert(memcmp(msg.muse_setup_tx.data, fx->data + 3, fx->length - 3) == 0);

  cl_assert_equal_i(prv_parse_fixture("muse_setup_tx_max", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.muse_setup_tx.length, AUDIO_COMPANION_MUSE_TX_MAX_BYTES);
  cl_assert_equal_i(msg.muse_setup_tx.data[AUDIO_COMPANION_MUSE_TX_MAX_BYTES - 1],
                    AUDIO_COMPANION_MUSE_TX_MAX_BYTES - 1);

  cl_assert_equal_i(prv_parse_fixture("muse_setup_disconnect", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.msg_id, AudioCompanionCtrlMsgIdMuseSetupDisconnect);
  cl_assert_equal_i(msg.muse_setup_disconnect.request_token, 0x3A);
  cl_assert_equal_i(msg.muse_setup_disconnect.delay_ms, 250);
}

void test_audio_companion_protocol__build_muse_setup_out(void) {
  uint8_t buf[AUDIO_COMPANION_MAX_CONTROL_WRITE_BYTES + 8];
  size_t len;

  static const char get_device_info[] = "{\"action\":\"get_device_info\"}";
  len = audio_companion_protocol_build_muse_setup_rx(buf, sizeof(buf), 0, 0,
                                                     (const uint8_t *)get_device_info,
                                                     sizeof(get_device_info) - 1);
  prv_assert_builder_matches("muse_setup_rx_get_device_info", buf, len);

  // The 300-byte value of the fragment fixtures, split at the 249-byte limit.
  uint8_t value[300];
  for (size_t i = 0; i < sizeof(value); i++) {
    value[i] = (uint8_t)((i * 7) & 0xFF);
  }
  len = audio_companion_protocol_build_muse_setup_rx(buf, sizeof(buf), 1,
                                                     AUDIO_COMPANION_MUSE_RX_FLAG_MORE, value,
                                                     AUDIO_COMPANION_MUSE_RX_MAX_BYTES);
  prv_assert_builder_matches("muse_setup_rx_fragment_more", buf, len);
  len = audio_companion_protocol_build_muse_setup_rx(
      buf, sizeof(buf), 1, 0, value + AUDIO_COMPANION_MUSE_RX_MAX_BYTES,
      sizeof(value) - AUDIO_COMPANION_MUSE_RX_MAX_BYTES);
  prv_assert_builder_matches("muse_setup_rx_fragment_last", buf, len);

  len = audio_companion_protocol_build_muse_setup_event(buf, sizeof(buf),
                                                        AudioCompanionMuseEventOpened, 0, 0, 300);
  prv_assert_builder_matches("muse_setup_event_opened", buf, len);
  len = audio_companion_protocol_build_muse_setup_event(
      buf, sizeof(buf), AudioCompanionMuseEventClientConnected, 0, 256, 287);
  prv_assert_builder_matches("muse_setup_event_connected", buf, len);
  len = audio_companion_protocol_build_muse_setup_event(
      buf, sizeof(buf), AudioCompanionMuseEventClosed, AudioCompanionMuseCloseReasonTimeout, 0, 0);
  prv_assert_builder_matches("muse_setup_event_closed_timeout", buf, len);
  len = audio_companion_protocol_build_muse_setup_event(
      buf, sizeof(buf), AudioCompanionMuseEventRxOverflow, 2, 256, 120);
  prv_assert_builder_matches("muse_setup_event_rx_overflow", buf, len);

  // A buffer too small for the value builds nothing.
  cl_assert_equal_i(audio_companion_protocol_build_muse_setup_rx(buf, 10, 0, 0, value, 20), 0);
}

// ---- Push-to-talk and watch actions ----

void test_audio_companion_protocol__parse_assistant_update(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("assistant_update_answer", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.msg_id, AudioCompanionCtrlMsgIdAssistantUpdate);
  cl_assert_equal_i(msg.assistant_update.request_token, 0x42);
  cl_assert_equal_i(msg.assistant_update.turn, 7);
  cl_assert_equal_i(msg.assistant_update.kind, AudioCompanionAssistantUpdateAnswer);
  const ProtocolFixture *fx = prv_find_fixture("assistant_update_answer");
  cl_assert_equal_i(msg.assistant_update.length, fx->length - 6);
  cl_assert(memcmp(msg.assistant_update.text, fx->data + 6, fx->length - 6) == 0);

  cl_assert_equal_i(prv_parse_fixture("assistant_update_error", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.assistant_update.kind, AudioCompanionAssistantUpdateError);
  cl_assert_equal_i(msg.assistant_update.detail, AudioCompanionAssistantErrorNotSetUp);
  cl_assert_equal_i(msg.assistant_update.length, 0);
}

void test_audio_companion_protocol__parse_watch_action_fragments(void) {
  AudioCompanionControlMsg msg;
  cl_assert_equal_i(prv_parse_fixture("watch_action_choose_first", &msg),
                    AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.msg_id, AudioCompanionCtrlMsgIdWatchAction);
  cl_assert_equal_i(msg.watch_action.action_id, 5);
  cl_assert_equal_i(msg.watch_action.flags, AUDIO_COMPANION_WATCH_ACTION_FLAG_MORE);
  cl_assert_equal_i(msg.watch_action.length, AUDIO_COMPANION_WATCH_ACTION_FRAGMENT_MAX_BYTES);
  cl_assert_equal_i(msg.watch_action.data[0], AudioCompanionWatchActionChoose);

  cl_assert_equal_i(prv_parse_fixture("watch_action_timer", &msg), AudioCompanionParseResultOk);
  cl_assert_equal_i(msg.watch_action.action_id, 3);
  cl_assert_equal_i(msg.watch_action.flags, 0);
  cl_assert_equal_i(msg.watch_action.data[0], AudioCompanionWatchActionTimer);
}

void test_audio_companion_protocol__build_push_to_talk_out(void) {
  uint8_t buf[64];
  size_t len;

  len = audio_companion_protocol_build_ptt_event(buf, sizeof(buf), 7, AudioCompanionPttEventStart,
                                                 AudioCompanionPttTriggerQuickLaunch, 0,
                                                 AUDIO_COMPANION_PTT_MAX_SECONDS);
  prv_assert_builder_matches("ptt_event_start", buf, len);
  len = audio_companion_protocol_build_ptt_event(buf, sizeof(buf), 7, AudioCompanionPttEventEnd,
                                                 AudioCompanionPttEndReleased, 212, 0);
  prv_assert_builder_matches("ptt_event_end", buf, len);
  len = audio_companion_protocol_build_ptt_event(buf, sizeof(buf), 9, AudioCompanionPttEventCancel,
                                                 AudioCompanionPttCancelTooShort, 9, 0);
  prv_assert_builder_matches("ptt_event_cancel_too_short", buf, len);

  len = audio_companion_protocol_build_moment_mark(buf, sizeof(buf), 3,
                                                  AUDIO_COMPANION_MOMENT_FLAG_RECORDING, 0x2A,
                                                  1234560, 1788193751778ULL);
  prv_assert_builder_matches("moment_mark_recording", buf, len);
  len = audio_companion_protocol_build_moment_mark(buf, sizeof(buf), 4, 0, 0, 0, 1788193811000ULL);
  prv_assert_builder_matches("moment_mark_not_recording", buf, len);
  cl_assert_equal_i(audio_companion_protocol_build_moment_mark(buf, 23, 4, 0, 0, 0, 1), 0);

  len = audio_companion_protocol_build_watch_action_result(
      buf, sizeof(buf), 5, AudioCompanionWatchActionStatusDone, 1, NULL);
  prv_assert_builder_matches("watch_action_result_chosen", buf, len);
  len = audio_companion_protocol_build_watch_action_result(
      buf, sizeof(buf), 5, AudioCompanionWatchActionStatusDeclined, 0, NULL);
  prv_assert_builder_matches("watch_action_result_declined", buf, len);
  len = audio_companion_protocol_build_watch_action_result(
      buf, sizeof(buf), 6, AudioCompanionWatchActionStatusDone, 0, "Clair de lune\nDebussy");
  prv_assert_builder_matches("watch_action_result_text", buf, len);

  // PTT_AUDIO: the header, then STREAM_DATA-style frame entries.
  uint8_t audio[128];
  len = audio_companion_protocol_build_ptt_audio_header(audio, sizeof(audio), 7, 0, 2);
  cl_assert_equal_i(len, sizeof(AudioCompanionPttAudioHeader));
  const uint8_t lengths[2] = {25, 27};
  for (uint32_t i = 0; i < 2; i++) {
    const uint16_t frame_len = lengths[i];
    memcpy(audio + len, &frame_len, sizeof(frame_len));
    len += sizeof(frame_len);
    uint8_t payload[AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES];
    prv_fill_frame_payload(i, frame_len, payload);
    memcpy(audio + len, payload, frame_len);
    len += frame_len;
  }
  prv_assert_builder_matches("ptt_audio_2frames", audio, len);
}
