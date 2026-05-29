/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/app_audio_context_private.h"

void test_app_audio_context_protocol__sizes(void) {
  cl_assert_equal_i(app_audio_context_header_size(), sizeof(AppAudioContextHeader));
  cl_assert_equal_i(app_audio_context_status_response_msg_size(),
                    sizeof(AppAudioContextStatusResponseMsg));
  cl_assert_equal_i(app_audio_context_transcript_response_header_size(),
                    sizeof(AppAudioContextTranscriptResponseHeader));
}

void test_app_audio_context_protocol__command_ids_match_mobile_contract(void) {
  cl_assert_equal_i(AppAudioContextMsgIdStatusRequest, 0x01);
  cl_assert_equal_i(AppAudioContextMsgIdStatusResponse, 0x02);
  cl_assert_equal_i(AppAudioContextMsgIdEnablePromptRequest, 0x03);
  cl_assert_equal_i(AppAudioContextMsgIdPromptResponse, 0x04);
  cl_assert_equal_i(AppAudioContextMsgIdPermissionRequest, 0x05);
  cl_assert_equal_i(AppAudioContextMsgIdTranscriptRequest, 0x06);
  cl_assert_equal_i(AppAudioContextMsgIdTranscriptResponse, 0x07);
  cl_assert_equal_i(AppAudioContextMsgIdSubscribeRequest, 0x08);
  cl_assert_equal_i(AppAudioContextMsgIdEvent, 0x09);
  cl_assert_equal_i(AppAudioContextMsgIdCancelRequest, 0x0a);
  cl_assert_equal_i(AppAudioContextMsgIdErrorResponse, 0x0b);
}

void test_app_audio_context_protocol__transcript_request_contains_common_header(void) {
  AppAudioContextTranscriptRequestMsg msg = {
    .header = {
      .command_id = AppAudioContextMsgIdTranscriptRequest,
      .protocol_version = APP_AUDIO_CONTEXT_PROTOCOL_VERSION,
      .request_id = 42,
    },
    .before_seconds = 60,
    .after_seconds = 5,
  };

  cl_assert_equal_i(msg.header.command_id, AppAudioContextMsgIdTranscriptRequest);
  cl_assert_equal_i(msg.header.protocol_version, APP_AUDIO_CONTEXT_PROTOCOL_VERSION);
  cl_assert_equal_i(msg.header.request_id, 42);
  cl_assert_equal_i(msg.before_seconds, 60);
  cl_assert_equal_i(msg.after_seconds, 5);
}
