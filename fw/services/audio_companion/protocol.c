/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/audio_companion_private.h"

#include <string.h>

_Static_assert(sizeof(AudioCompanionInfo) == AUDIO_COMPANION_INFO_SIZE, "info size");
_Static_assert(sizeof(AudioCompanionAuthRequestHeader) == 36, "auth request header size");
_Static_assert(sizeof(AudioCompanionAuthRevokeMsg) == 34, "auth revoke size");
_Static_assert(sizeof(AudioCompanionCheckpointMsg) == 26, "checkpoint size");
_Static_assert(sizeof(AudioCompanionPauseRequestMsg) == 3, "pause request size");
_Static_assert(sizeof(AudioCompanionResumeRequestMsg) == 2, "resume request size");
_Static_assert(sizeof(AudioCompanionEnableRequestMsg) == 2, "enable request size");
_Static_assert(sizeof(AudioCompanionReceiverHealthMsg) == 8, "receiver health size");
_Static_assert(sizeof(AudioCompanionPhoneMediaStateMsg) == 3, "phone media state size");
_Static_assert(sizeof(AudioCompanionPhoneMediaQueryMsg) == 1, "phone media query size");
_Static_assert(sizeof(AudioCompanionMuseSetupOpenMsg) == 11, "muse setup open size");
_Static_assert(sizeof(AudioCompanionMuseSetupCloseMsg) == 3, "muse setup close size");
_Static_assert(sizeof(AudioCompanionMuseSetupTxHeader) == 3, "muse setup tx header size");
_Static_assert(sizeof(AudioCompanionMuseSetupDisconnectMsg) == 4, "muse setup disconnect size");
_Static_assert(sizeof(AudioCompanionMuseSetupRxHeader) == 4, "muse setup rx header size");
_Static_assert(sizeof(AudioCompanionMuseSetupEventMsg) == 7, "muse setup event size");
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
    case AudioCompanionCtrlMsgIdEnableRequest:
      if (length < sizeof(AudioCompanionEnableRequestMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->enable_request, data, sizeof(msg_out->enable_request));
      break;
    case AudioCompanionCtrlMsgIdReceiverHealth:
      if (length < sizeof(AudioCompanionReceiverHealthMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->receiver_health, data, sizeof(msg_out->receiver_health));
      break;
    case AudioCompanionCtrlMsgIdPhoneMediaState:
      if (length < sizeof(AudioCompanionPhoneMediaStateMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->phone_media_state, data, sizeof(msg_out->phone_media_state));
      break;
    case AudioCompanionCtrlMsgIdMuseSetupOpen:
      if (length < sizeof(AudioCompanionMuseSetupOpenMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->muse_setup_open, data, sizeof(msg_out->muse_setup_open));
      break;
    case AudioCompanionCtrlMsgIdMuseSetupClose:
      if (length < sizeof(AudioCompanionMuseSetupCloseMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->muse_setup_close, data, sizeof(msg_out->muse_setup_close));
      break;
    case AudioCompanionCtrlMsgIdMuseSetupTx: {
      AudioCompanionMuseSetupTxHeader header;
      if (length < sizeof(header)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&header, data, sizeof(header));
      if (header.length == 0 || header.length > AUDIO_COMPANION_MUSE_TX_MAX_BYTES ||
          length < sizeof(header) + header.length) {
        return AudioCompanionParseResultMalformed;
      }
      msg_out->muse_setup_tx.request_token = header.request_token;
      msg_out->muse_setup_tx.length = header.length;
      memcpy(msg_out->muse_setup_tx.data, data + sizeof(header), header.length);
      break;
    }
    case AudioCompanionCtrlMsgIdMuseSetupDisconnect:
      if (length < sizeof(AudioCompanionMuseSetupDisconnectMsg)) {
        return AudioCompanionParseResultMalformed;
      }
      memcpy(&msg_out->muse_setup_disconnect, data, sizeof(msg_out->muse_setup_disconnect));
      break;
    default:
      return AudioCompanionParseResultUnknown;
  }

  msg_out->msg_id = msg_id;
  return AudioCompanionParseResultOk;
}

size_t audio_companion_protocol_build_info(uint8_t *buf, size_t buf_size,
                                           const AudioCompanionInfo *info) {
  // A version-1 snapshot stays byte-exact at 20 bytes; version 2 appends the restart fields.
  const size_t size = (info && info->info_version >= 2) ? sizeof(*info)
                                                        : AUDIO_COMPANION_INFO_V1_SIZE;
  if (!buf || !info || buf_size < size) {
    return 0;
  }
  memcpy(buf, info, size);
  return size;
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

size_t audio_companion_protocol_build_phone_media_query(uint8_t *buf, size_t buf_size) {
  if (!buf || buf_size < sizeof(AudioCompanionPhoneMediaQueryMsg)) {
    return 0;
  }
  const AudioCompanionPhoneMediaQueryMsg msg = {
    .msg_id = AudioCompanionCtrlMsgIdPhoneMediaQuery,
  };
  memcpy(buf, &msg, sizeof(msg));
  return sizeof(msg);
}

size_t audio_companion_protocol_build_muse_setup_rx(uint8_t *buf, size_t buf_size, uint8_t seq,
                                                    uint8_t flags, const uint8_t *data,
                                                    uint8_t length) {
  const size_t total = sizeof(AudioCompanionMuseSetupRxHeader) + length;
  if (!buf || !data || buf_size < total) {
    return 0;
  }
  const AudioCompanionMuseSetupRxHeader header = {
    .msg_id = AudioCompanionCtrlMsgIdMuseSetupRx,
    .seq = seq,
    .flags = flags,
    .length = length,
  };
  memcpy(buf, &header, sizeof(header));
  memcpy(buf + sizeof(header), data, length);
  return total;
}

size_t audio_companion_protocol_build_muse_setup_event(uint8_t *buf, size_t buf_size,
                                                       uint8_t event, uint8_t detail,
                                                       uint16_t mtu, uint16_t seconds_left) {
  if (!buf || buf_size < sizeof(AudioCompanionMuseSetupEventMsg)) {
    return 0;
  }
  const AudioCompanionMuseSetupEventMsg msg = {
    .msg_id = AudioCompanionCtrlMsgIdMuseSetupEvent,
    .event = event,
    .detail = detail,
    .mtu = mtu,
    .seconds_left = seconds_left,
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
