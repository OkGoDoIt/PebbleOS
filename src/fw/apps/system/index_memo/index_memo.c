/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "index_memo.h"

#include "applib/app.h"
#include "applib/ui/app_window_stack.h"
#include "applib/voice/dictation_session.h"
#include "applib/voice/dictation_session_private.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/voice_endpoint_private.h"
#include "system/logging.h"

#define INDEX_MEMO_BUFFER_SIZE 32

static DictationSession *s_session;

static void prv_handle_result(DictationSession *session, DictationSessionStatus status,
                              char *transcription, void *context) {
  (void)transcription;
  (void)context;
  PBL_LOG_INFO("Index Memo dictation finished with status %d", (int)status);
  dictation_session_destroy(session);
  s_session = NULL;
  app_window_stack_pop_all(false);
}

static void prv_main(void) {
  s_session = dictation_session_create(INDEX_MEMO_BUFFER_SIZE, prv_handle_result, NULL);
  if (!s_session) {
    PBL_LOG_ERR("Index Memo failed to create dictation session");
    app_window_stack_pop_all(false);
    return;
  }

  dictation_session_set_session_intent(s_session, VoiceEndpointSessionIntentIndexMemo);
  dictation_session_enable_confirmation(s_session, false);
  dictation_session_enable_error_dialogs(s_session, true);

  DictationSessionStatus status = dictation_session_start(s_session);
  if (status != DictationSessionStatusSuccess) {
    PBL_LOG_ERR("Index Memo failed to start dictation session: %d", (int)status);
    dictation_session_destroy(s_session);
    s_session = NULL;
    app_window_stack_pop_all(false);
    return;
  }

  app_event_loop();
}

const PebbleProcessMd *index_memo_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common = {
      .main_func = &prv_main,
      .uuid = INDEX_MEMO_UUID,
      .visibility = ProcessVisibilityQuickLaunch,
    },
    .name = i18n_noop("Index Memo"),
  };
  return &s_app_info.common;
}
