/* SPDX-License-Identifier: Apache-2.0 */

#ifdef CONFIG_SERVICE_AUDIO_COMPANION

#include "audio_companion_consent_ui.h"

#include "applib/ui/action_bar_layer.h"
#include "applib/ui/dialogs/confirmation_dialog.h"
#include "applib/ui/dialogs/dialog.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/modals/modal_manager.h"
#include "pbl/services/audio_companion.h"
#include "pbl/services/i18n/i18n.h"
#include "resource/resource_ids.auto.h"
#include "util/string.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  ConfirmationDialog *dialog;
  bool responded;
} AudioCompanionConsentPrompt;

static AudioCompanionConsentPrompt *s_prompt;

static void prv_finish_prompt(AudioCompanionConsentPrompt *prompt, bool granted) {
  if (!prompt || prompt->responded) {
    return;
  }

  prompt->responded = true;
  audio_companion_handle_consent_response(granted);
  confirmation_dialog_pop(prompt->dialog);
}

static void prv_click_handler(ClickRecognizerRef recognizer, void *context) {
  AudioCompanionConsentPrompt *prompt = context;
  const bool granted = (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
  prv_finish_prompt(prompt, granted);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_click_handler);
}

static void prv_dialog_unload(void *context) {
  AudioCompanionConsentPrompt *prompt = context;
  if (prompt && !prompt->responded) {
    audio_companion_handle_consent_response(false);
  }
  if (s_prompt == prompt) {
    s_prompt = NULL;
  }
  task_free(prompt);
}

static void prv_show_consent_prompt(const char *receiver_name) {
  if (s_prompt) {
    audio_companion_handle_consent_response(false);
    return;
  }

  AudioCompanionConsentPrompt *prompt = task_zalloc_check(sizeof(*prompt));
  ConfirmationDialog *confirmation_dialog = confirmation_dialog_create("Audio Companion");
  prompt->dialog = confirmation_dialog;

  Dialog *dialog = confirmation_dialog_get_dialog(confirmation_dialog);
  const char *fmt = i18n_get("Allow %s to stream background audio?", prompt);
  char *msg = task_zalloc_check(DIALOG_MAX_MESSAGE_LEN);
  sniprintf(msg, DIALOG_MAX_MESSAGE_LEN, fmt,
            (receiver_name && receiver_name[0]) ? receiver_name : "Audio app");
  dialog_set_text(dialog, msg);
  dialog_set_background_color(dialog, GColorCobaltBlue);
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_icon(dialog, RESOURCE_ID_VOICE_MICROPHONE_LARGE);
  dialog_set_timeout(dialog, AUDIO_COMPANION_CONSENT_TIMEOUT_SECONDS * 1000);
  const DialogCallbacks callbacks = {
    .unload = prv_dialog_unload,
  };
  dialog_set_callbacks(dialog, &callbacks, prompt);

  confirmation_dialog_set_click_config_provider(confirmation_dialog, prv_click_config_provider);
  ActionBarLayer *action_bar = confirmation_dialog_get_action_bar(confirmation_dialog);
  action_bar_layer_set_context(action_bar, prompt);

  s_prompt = prompt;
  i18n_free_all(prompt);
  task_free(msg);
  confirmation_dialog_push(confirmation_dialog, modal_manager_get_window_stack(ModalPriorityGeneric));
}

void audio_companion_consent_ui_init(void) {
  audio_companion_set_consent_handler(prv_show_consent_prompt);
}

#else

void audio_companion_consent_ui_init(void) {
}

#endif
