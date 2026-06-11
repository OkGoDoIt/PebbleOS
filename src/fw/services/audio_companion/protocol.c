/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/audio_companion_private.h"

#include <string.h>

_Static_assert(sizeof(AudioCompanionInfo) == AUDIO_COMPANION_INFO_SIZE, "info size");
_Static_assert(sizeof(AudioCompanionAuthRequestHeader) == 36, "auth request header size");
_Static_assert(sizeof(AudioCompanionAuthRevokeMsg) == 34, "auth revoke size");
_Static_assert(sizeof(AudioCompanionCheckpointMsg) == 26, "checkpoint size");
_Static_assert(sizeof(AudioCompanionPauseRequestMsg) == 3, "pause request size");
_Static_assert(sizeof(AudioCompanionResumeRequestMsg) == 2, "resume request size");
_Static_assert(sizeof(AudioCompanionReceiverHealthMsg) == 8, "receiver health size");
_Static_assert(sizeof(AudioCompanionAuthResultMsg) == 4, "auth result size");
_Static_assert(sizeof(AudioCompanionRevokedMsg) == 2, "revoked size");
_Static_assert(sizeof(AudioCompanionAckMsg) == 3, "ack size");
_Static_assert(sizeof(AudioCompanionStateChangedMsg) == 2, "state changed size");
_Static_assert(sizeof(AudioCompanionErrorMsg) == 6, "error size");
_Static_assert(sizeof(AudioCompanionStreamStartMsg) == 40, "stream start size");
_Static_assert(sizeof(AudioCompanionStreamDataHeader) == 20, "stream data header size");
_Static_assert(sizeof(AudioCompanionStreamGapMsg) == 26, "stream gap size");
_Static_assert(sizeof(AudioCompanionStreamStopMsg) == 22, "stream stop size");

AudioCompanionParseResult audio_companion_protocol_parse_control(
    const uint8_t *data, size_t length, AudioCompanionControlMsg *msg_out) {
  if (!data || !msg_out || length < 1) {
    return AudioCompanionParseResultMalformed;
  }

  const uint8_t msg_id = data[0];
  switch (msg_id) {
    case AudioCompanionCtrlMsgIdAuthRequest: {
      if (length < sizeof(AudioCompanionAuthRequestHeader)) {
        return AudioCompanionParseResultMalformed;
      }
      AudioCompanionAuthRequestHeader header;
      memcpy(&header, data, sizeof(header));
      if (header.name_len > AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES ||
          length < sizeof(header) + header.name_len) {
        return AudioCompanionParseResultMalformed;
      }
      AudioCompanionAuthRequest *req = &msg_out->auth_request;
      memset(req, 0, sizeof(*req));
      req->proto_version = header.proto_version;
      req->request_token = header.request_token;
      memcpy(req->receiver_id, header.receiver_id, sizeof(req->receiver_id));
      req->name_len = header.name_len;
      memcpy(req->name, data + sizeof(header), header.name_len);
      req->name[header.name_len] = '\0';
      break;
    }
    case AudioCompanionCtrlMsgIdAuthRevoke:
      if (length < sizeof(AudioCompanionAuthRevokeMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->auth_revoke, data, sizeof(msg_out->auth_revoke));
      break;
    case AudioCompanionCtrlMsgIdCheckpoint:
      if (length < sizeof(AudioCompanionCheckpointMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->checkpoint, data, sizeof(msg_out->checkpoint));
      break;
    case AudioCompanionCtrlMsgIdPauseRequest:
      if (length < sizeof(AudioCompanionPauseRequestMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->pause_request, data, sizeof(msg_out->pause_request));
      break;
    case AudioCompanionCtrlMsgIdResumeRequest:
      if (length < sizeof(AudioCompanionResumeRequestMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->resume_request, data, sizeof(msg_out->resume_request));
      break;
    case AudioCompanionCtrlMsgIdReceiverHealth:
      if (length < sizeof(AudioCompanionReceiverHealthMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->receiver_health, data, sizeof(msg_out->receiver_health));
      break;
    default:
      return AudioCompanionParseResultUnknown;
  }

  msg_out->msg_id = msg_id;
  return AudioCompanionParseResultOk;
}

size_t audio_companion_protocol_build_info(uint8_t *buf, size_t buf_size,
                                           const AudioCompanionInfo *info) {
  if (!buf || !info || buf_size < sizeof(*info)) {
    return 0;
  }
  memcpy(buf, info, sizeof(*info));
  return sizeof(*info);
}

size_t audio_companion_protocol_build_auth_result(uint8_t *buf, size_t buf_size,
                                                  uint8_t request_token, uint8_t status,
                                                  uint8_t granted_proto_version) {
  if (!buf || buf_size < sizeof(AudioCompanionAuthResultMsg)) {
    return 0;
  }
  const AudioCompanionAuthResultMsg msg = {
    .msg_id = AudioCompanionCtrlMsgIdAuthResult,
    .request_token = request_token,
    .status = status,
    .granted_proto_version = granted_proto_version,
  };
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}

size_t audio_companion_protocol_build_revoked(uint8_t *buf, size_t buf_size, uint8_t reason) {
  if (!buf || buf_size < sizeof(AudioCompanionRevokedMsg)) {
    return 0;
  }
  const AudioCompanionRevokedMsg msg = {
    .msg_id = AudioCompanionCtrlMsgIdRevoked,
    .reason = reason,
  };
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}

size_t audio_companion_protocol_build_ack(uint8_t *buf, size_t buf_size, uint8_t request_token,
                                          uint8_t status) {
  if (!buf || buf_size < sizeof(AudioCompanionAckMsg)) {
    return 0;
  }
  const AudioCompanionAckMsg msg = {
    .msg_id = AudioCompanionCtrlMsgIdAck,
    .request_token = request_token,
    .status = status,
  };
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}

size_t audio_companion_protocol_build_state_changed(uint8_t *buf, size_t buf_size,
                                                    uint8_t service_state) {
  if (!buf || buf_size < sizeof(AudioCompanionStateChangedMsg)) {
    return 0;
  }
  const AudioCompanionStateChangedMsg msg = {
    .msg_id = AudioCompanionCtrlMsgIdStateChanged,
    .service_state = service_state,
  };
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}

size_t audio_companion_protocol_build_error(uint8_t *buf, size_t buf_size, uint8_t error_code,
                                            uint32_t detail) {
  if (!buf || buf_size < sizeof(AudioCompanionErrorMsg)) {
    return 0;
  }
  const AudioCompanionErrorMsg msg = {
    .msg_id = AudioCompanionCtrlMsgIdError,
    .error_code = error_code,
    .detail = detail,
  };
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}

size_t audio_companion_protocol_build_stream_start(uint8_t *buf, size_t buf_size,
                                                   const AudioCompanionStreamStartMsg *params) {
  if (!buf || !params || buf_size < sizeof(*params)) {
    return 0;
  }
  AudioCompanionStreamStartMsg msg = *params;
  msg.msg_id = AudioCompanionDataMsgIdStreamStart;
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}

size_t audio_companion_protocol_build_stream_data_header(uint8_t *buf, size_t buf_size,
                                                         uint32_t stream_id,
                                                         uint32_t first_sequence,
                                                         uint64_t first_sample_index,
                                                         uint8_t frame_count, uint16_t flags) {
  if (!buf || buf_size < sizeof(AudioCompanionStreamDataHeader) || frame_count == 0 ||
      frame_count > AUDIO_COMPANION_MAX_FRAMES_PER_DATA_MSG) {
    return 0;
  }
  const AudioCompanionStreamDataHeader header = {
    .msg_id = AudioCompanionDataMsgIdStreamData,
    .stream_id = stream_id,
    .first_sequence = first_sequence,
    .first_sample_index = first_sample_index,
    .frame_count = frame_count,
    .flags = flags,
  };
  memcpy(buf, &header, sizeof(header));
  return sizeof(header);
}

size_t audio_companion_protocol_build_stream_gap(uint8_t *buf, size_t buf_size,
                                                 const AudioCompanionStreamGapMsg *params) {
  if (!buf || !params || buf_size < sizeof(*params)) {
    return 0;
  }
  AudioCompanionStreamGapMsg msg = *params;
  msg.msg_id = AudioCompanionDataMsgIdStreamGap;
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}

size_t audio_companion_protocol_build_stream_stop(uint8_t *buf, size_t buf_size,
                                                  const AudioCompanionStreamStopMsg *params) {
  if (!buf || !params || buf_size < sizeof(*params)) {
    return 0;
  }
  AudioCompanionStreamStopMsg msg = *params;
  msg.msg_id = AudioCompanionDataMsgIdStreamStop;
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}
