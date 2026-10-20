/* SPDX-License-Identifier: Apache-2.0 */

#include "audio_companion_muse_ui.h"

#ifdef CONFIG_SERVICE_AUDIO_COMPANION_MUSE

#include "applib/ui/action_bar_layer.h"
#include "applib/ui/dialogs/confirmation_dialog.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/dialogs/dialog_private.h"
#include "applib/ui/dialogs/simple_dialog.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "kernel/pebble_tasks.h"
#include "kernel/ui/modals/modal_manager.h"
#include "pbl/logging/logging.h"
#include "pbl/services/audio_companion_private.h"
#include "pbl/services/i18n/i18n.h"
#include "resource/resource_ids.auto.h"
#include "services/audio_companion/muse_setup_relay.h"
#include "system/passert.h"

#include <string.h>

PBL_LOG_MODULE_DECLARE(service_audio_companion, CONFIG_SERVICE_AUDIO_COMPANION_LOG_LEVEL);

//! The prompt gets the same patience as the audio consent prompt.
#define MUSE_CONSENT_TIMEOUT_MS (AUDIO_COMPANION_CONSENT_TIMEOUT_SECONDS * 1000)
//! How long an ending stays on screen.
#define MUSE_ENDING_TIMEOUT_MS (4000)

typedef struct {
  ConfirmationDialog *dialog;
  bool responded;
} MuseConsentPrompt;

typedef struct {
  SimpleDialog *dialog;
  //! Set before the card is popped by this code, so its unload does not read as the user leaving.
  bool popped_by_us;
  bool ending;
} MuseStatusCard;

static MuseConsentPrompt *s_prompt;
static MuseStatusCard *s_card;

// ---- consent prompt ----

static void prv_finish_prompt(MuseConsentPrompt *prompt, bool granted) {
  if (!prompt || prompt->responded) {
    return;
  }
  prompt->responded = true;
  audio_companion_muse_consent_response(granted);
  confirmation_dialog_pop(prompt->dialog);
}

static void prv_prompt_click_handler(ClickRecognizerRef recognizer, void *context) {
  MuseConsentPrompt *prompt = context;
  prv_finish_prompt(prompt, click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
}

static void prv_prompt_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_prompt_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_prompt_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_prompt_click_handler);
}

static void prv_prompt_unload(void *context) {
  MuseConsentPrompt *prompt = context;
  if (prompt && !prompt->responded) {
    audio_companion_muse_consent_response(false);  // timed out, or dismissed some other way
  }
  if (s_prompt == prompt) {
    s_prompt = NULL;
  }
  task_free(prompt);
}

static void prv_show_prompt_kernel_main_cb(void *data) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  if (s_prompt) {
    PBL_LOG_INFO("Muse prompt busy; declining");
    audio_companion_muse_consent_response(false);
    return;
  }

  MuseConsentPrompt *prompt = task_zalloc_check(sizeof(*prompt));
  ConfirmationDialog *confirmation = confirmation_dialog_create("Muse");
  prompt->dialog = confirmation;

  Dialog *dialog = confirmation_dialog_get_dialog(confirmation);
  dialog_set_text(dialog, i18n_get("Pair with Muse on your phone?", prompt));
  dialog_set_background_color(dialog, GColorCobaltBlue);
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_icon(dialog, RESOURCE_ID_GENERIC_QUESTION_LARGE);
  dialog_set_timeout(dialog, MUSE_CONSENT_TIMEOUT_MS);
  const DialogCallbacks callbacks = {.unload = prv_prompt_unload};
  dialog_set_callbacks(dialog, &callbacks, prompt);

  confirmation_dialog_set_click_config_provider(confirmation, prv_prompt_click_config_provider);
  action_bar_layer_set_context(confirmation_dialog_get_action_bar(confirmation), prompt);

  s_prompt = prompt;
  i18n_free_all(prompt);
  confirmation_dialog_push(confirmation, modal_manager_get_window_stack(ModalPriorityGeneric));
}

// ---- status card ----

static void prv_card_unload(void *context) {
  MuseStatusCard *card = context;
  if (card && !card->popped_by_us && !card->ending) {
    // The user left the card (Back, or any button) while the window was open: that is a cancel.
    audio_companion_muse_cancel_from_watch();
  }
  if (s_card == card) {
    s_card = NULL;
  }
  task_free(card);
}

static void prv_pop_card(void) {
  if (!s_card) {
    return;
  }
  s_card->popped_by_us = true;
  dialog_pop(simple_dialog_get_dialog(s_card->dialog));
}

static void prv_push_card(const char *text, bool ending) {
  prv_pop_card();
  MuseStatusCard *card = task_zalloc_check(sizeof(*card));
  card->ending = ending;
  card->dialog = simple_dialog_create("Muse");
  Dialog *dialog = simple_dialog_get_dialog(card->dialog);
  dialog_set_text(dialog, text);
  dialog_set_background_color(dialog, GColorCobaltBlue);
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_icon(dialog, ending ? RESOURCE_ID_GENERIC_CONFIRMATION_LARGE
                                 : RESOURCE_ID_RESULT_SENT_LARGE);
  if (ending) {
    dialog_set_timeout(dialog, MUSE_ENDING_TIMEOUT_MS);
  }
  const DialogCallbacks callbacks = {.unload = prv_card_unload};
  dialog_set_callbacks(dialog, &callbacks, card);
  s_card = card;
  simple_dialog_push(card->dialog, modal_manager_get_window_stack(ModalPriorityGeneric));
}

static const char *prv_ending_text(uint8_t reason) {
  switch (reason) {
    case AudioCompanionMuseCloseReasonAppPaired: return "Paired with Muse";
    case AudioCompanionMuseCloseReasonTimeout: return "Muse didn't connect";
    case AudioCompanionMuseCloseReasonPhoneLinkLost: return "Phone disconnected";
    case AudioCompanionMuseCloseReasonCouldNotStart: return "Couldn't start pairing";
    case AudioCompanionMuseCloseReasonAppFailed:
    case AudioCompanionMuseCloseReasonReceiverGone: return "Pairing didn't finish";
    // The user said no, or left the card: nothing more to say.
    case AudioCompanionMuseCloseReasonDeclinedOnWatch:
    case AudioCompanionMuseCloseReasonCancelledOnWatch: return NULL;
    default: return "Pairing cancelled";
  }
}

typedef struct {
  AudioCompanionMuseStatus status;
  uint8_t detail;
} MuseStatusUpdate;

static void prv_status_kernel_main_cb(void *data) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  MuseStatusUpdate *update = data;
  switch (update->status) {
    case AudioCompanionMuseStatusWaiting:
      prv_push_card("Waiting for Muse\n\nOn your phone: Muse > Settings > Devices > Add", false);
      break;
    case AudioCompanionMuseStatusConnected:
      prv_push_card("Muse is connected\nFinishing setup...", false);
      break;
    case AudioCompanionMuseStatusClosed: {
      if (s_prompt) {
        prv_finish_prompt(s_prompt, false);  // the request went away while the prompt was up
      }
      const char *text = prv_ending_text(update->detail);
      if (text) {
        prv_push_card(text, true);
      } else {
        prv_pop_card();
      }
      break;
    }
  }
  kernel_free(update);
}

// ---- relay handlers (system task): defer to KernelMain ----

static void prv_consent_handler(void) {
  launcher_task_add_callback(prv_show_prompt_kernel_main_cb, NULL);
}

static void prv_status_handler(AudioCompanionMuseStatus status, uint8_t detail) {
  MuseStatusUpdate *update = kernel_malloc(sizeof(*update));
  if (!update) {
    return;
  }
  *update = (MuseStatusUpdate){.status = status, .detail = detail};
  launcher_task_add_callback(prv_status_kernel_main_cb, update);
}

void audio_companion_muse_ui_init(void) {
  audio_companion_muse_set_ui_handlers(prv_consent_handler, prv_status_handler);
}

#else

void audio_companion_muse_ui_init(void) {
}

#endif
