/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "voice_window.h"
#include "dictation_session.h"
#include "pbl/services/voice_endpoint_private.h"
#include "applib/event_service_client.h"

#include <stdint.h>
#include <stdbool.h>

struct DictationSession {
  VoiceWindow *voice_window;
  DictationSessionStatusCallback callback;
  void *context;
  bool in_progress;
  bool destroy_pending;
  EventServiceInfo dictation_result_sub;
  EventServiceInfo app_focus_sub;
};

void dictation_session_set_session_intent(DictationSession *session,
                                          VoiceEndpointSessionIntent session_intent);
