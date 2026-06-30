/* SPDX-License-Identifier: Apache-2.0 */

#ifdef CONFIG_SERVICE_AUDIO_COMPANION

#include "audio_companion.h"
#include "window.h"

#include "applib/app_timer.h"
#include "applib/ui/action_bar_layer.h"
#include "applib/ui/dialogs/confirmation_dialog.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/dialogs/expandable_dialog.h"
#include "applib/ui/menu_layer.h"
#include "kernel/pbl_malloc.h"
#include "pbl/services/audio_companion.h"
#include "pbl/services/i18n/i18n.h"
#include "services/audio_companion/reboot_trace.h"
#include "resource/resource_ids.auto.h"
#include "system/passert.h"

#include <inttypes.h>
#include <stdio.h>

typedef enum {
  AudioCompanionSettingsToggle,
  AudioCompanionSettingsStatus,
  AudioCompanionSettingsSkipSilence,
  AudioCompanionSettingsReceiver,
  AudioCompanionSettingsCount,
} AudioCompanionSettingsItem;

typedef struct {
  SettingsCallbacks callbacks;
  AppTimer *update_timer;
} SettingsAudioCompanionData;

#define STATUS_UPDATE_INTERVAL_MS (500)

static const char *prv_state_name(AudioCompanionServiceState state) {
  switch (state) {
    case AudioCompanionServiceStateDisabled:
      return i18n_noop("Disabled");
    case AudioCompanionServiceStateIdle:
      return i18n_noop("Idle");
    case AudioCompanionServiceStateAuthorizedIdle:
      return i18n_noop("Ready");
    case AudioCompanionServiceStateStreaming:
      return i18n_noop("Streaming");
    case AudioCompanionServiceStatePausedConflict:
      return i18n_noop("Mic in Use");
    case AudioCompanionServiceStatePausedPolicy:
      return i18n_noop("Paused");
    case AudioCompanionServiceStatePausedLowBattery:
      return i18n_noop("Low Battery");
    case AudioCompanionServiceStatePausedPowerSave:
      return i18n_noop("Power Save");
    case AudioCompanionServiceStateError:
      return i18n_noop("Error");
    default:
      return i18n_noop("Unknown");
  }
}

static void prv_forget_receiver_confirmed(ClickRecognizerRef recognizer, void *context) {
  ConfirmationDialog *dialog = context;
  const bool confirmed = (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
  confirmation_dialog_pop(dialog);
  if (confirmed) {
    audio_companion_forget_receiver();
    settings_menu_reload_data(SettingsMenuItemAudioCompanion);
  }
}

static void prv_forget_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_forget_receiver_confirmed);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_forget_receiver_confirmed);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_forget_receiver_confirmed);
}

static void prv_show_forget_receiver_confirm(void *i18n_owner) {
  ConfirmationDialog *confirmation_dialog = confirmation_dialog_create("Forget Audio Receiver");
  Dialog *dialog = confirmation_dialog_get_dialog(confirmation_dialog);
  dialog_set_text(dialog, i18n_get("Forget the paired audio receiver?", i18n_owner));
  dialog_set_background_color(dialog, GColorRed);
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_icon(dialog, RESOURCE_ID_GENERIC_CONFIRMATION_LARGE);
  confirmation_dialog_set_click_config_provider(confirmation_dialog, prv_forget_click_config_provider);
  ActionBarLayer *action_bar = confirmation_dialog_get_action_bar(confirmation_dialog);
  action_bar_layer_set_context(action_bar, confirmation_dialog);
  app_confirmation_dialog_push(confirmation_dialog);
}

static void prv_diagnostics_close(ClickRecognizerRef recognizer, void *context) {
  expandable_dialog_pop(context);
}

static void prv_show_diagnostics(SettingsAudioCompanionData *data) {
  AudioCompanionDiagnostics diag;
  audio_companion_get_diagnostics(&diag);

  char *text = app_zalloc_check(DIALOG_MAX_MESSAGE_LEN);
  int written = sniprintf(text, DIALOG_MAX_MESSAGE_LEN,
            "State: %s\nCaptured: %" PRIu32 "\nSent: %" PRIu32 "\nBuffered: %" PRIu32
            " B\nDropped: %" PRIu32 "\nGaps: %" PRIu32 "\nBackpressure: %" PRIu32,
            i18n_get(prv_state_name(diag.state), data), diag.captured_frames, diag.sent_frames,
            diag.spool_bytes, diag.dropped_overflow_frames, diag.gap_records,
            diag.send_backpressure_events);

  // Reboot flight recorder: shows why the watch last restarted so an unexpected reboot can be
  // traced to a fault class (Watchdog / Event Queue Full / Out of Memory / ...) after the fact.
  AudioCompanionRebootTrace trace;
  audio_companion_get_reboot_trace(&trace);
  const AudioCompanionRebootTraceEntry *last = audio_companion_reboot_trace_newest(&trace);
  if (last && written > 0 && written < DIALOG_MAX_MESSAGE_LEN) {
    sniprintf(text + written, DIALOG_MAX_MESSAGE_LEN - written,
              "\n\nLast restart: %s\nRestarts: %" PRIu16 " (faults %" PRIu16 ")",
              audio_companion_reboot_trace_reason_name(last->reason_code), trace.total_reboots,
              trace.total_error_reboots);
  }

  ExpandableDialog *dialog = expandable_dialog_create_with_params(
      "Audio Diagnostics", RESOURCE_ID_AUDIO_CASSETTE_LARGE, text, GColorBlack, GColorWhite,
      NULL, RESOURCE_ID_ACTION_BAR_ICON_CHECK, prv_diagnostics_close);
  expandable_dialog_show_action_bar(dialog, true);
  expandable_dialog_set_header(dialog, i18n_get("Diagnostics", dialog));
  app_expandable_dialog_push(dialog);
  app_free(text);
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsAudioCompanionData *data = (SettingsAudioCompanionData *)context;
  if (data->update_timer) {
    app_timer_cancel(data->update_timer);
    data->update_timer = NULL;
  }
  i18n_free_all(context);
  app_free(context);
}

static void prv_update_timer_cb(void *context) {
  SettingsAudioCompanionData *data = context;
  settings_menu_mark_dirty(SettingsMenuItemAudioCompanion);
  data->update_timer = app_timer_register(STATUS_UPDATE_INTERVAL_MS, prv_update_timer_cb, data);
}

static void prv_appear_cb(SettingsCallbacks *context) {
  SettingsAudioCompanionData *data = (SettingsAudioCompanionData *)context;
  if (!data->update_timer) {
    data->update_timer =
        app_timer_register(STATUS_UPDATE_INTERVAL_MS, prv_update_timer_cb, data);
  }
}

static void prv_hide_cb(SettingsCallbacks *context) {
  SettingsAudioCompanionData *data = (SettingsAudioCompanionData *)context;
  if (data->update_timer) {
    app_timer_cancel(data->update_timer);
    data->update_timer = NULL;
  }
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  return AudioCompanionSettingsCount;
}

static const char *prv_silence_mode_name(AudioCompanionSilenceMode mode) {
  switch (mode) {
    case AudioCompanionSilenceModeOff:
      return i18n_noop("Off");
    case AudioCompanionSilenceModeLight:
      return i18n_noop("Light");
    case AudioCompanionSilenceModeBalanced:
      return i18n_noop("Balanced");
    case AudioCompanionSilenceModeAggressive:
      return i18n_noop("Aggressive");
    case AudioCompanionSilenceModeCount:
      break;
  }
  return i18n_noop("Light");
}

static AudioCompanionSilenceMode prv_next_silence_mode(AudioCompanionSilenceMode mode) {
  switch (mode) {
    case AudioCompanionSilenceModeOff:
      return AudioCompanionSilenceModeLight;
    case AudioCompanionSilenceModeLight:
      return AudioCompanionSilenceModeBalanced;
    case AudioCompanionSilenceModeBalanced:
      return AudioCompanionSilenceModeAggressive;
    case AudioCompanionSilenceModeAggressive:
    case AudioCompanionSilenceModeCount:
      return AudioCompanionSilenceModeOff;
  }
  return AudioCompanionSilenceModeLight;
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx,
                            const Layer *cell_layer, uint16_t row, bool selected) {
  SettingsAudioCompanionData *data = (SettingsAudioCompanionData *)context;
  const char *title = NULL;
  const char *subtitle = NULL;
  char receiver_name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1] = {0};

  switch ((AudioCompanionSettingsItem)row) {
    case AudioCompanionSettingsToggle:
      title = i18n_noop("Background Audio");
      subtitle = audio_companion_is_enabled() ? i18n_noop("On") : i18n_noop("Off");
      break;
    case AudioCompanionSettingsStatus:
      title = i18n_noop("Status");
      subtitle = prv_state_name(audio_companion_get_state());
      break;
    case AudioCompanionSettingsSkipSilence:
      title = i18n_noop("Skip Silence");
      subtitle = prv_silence_mode_name(audio_companion_get_silence_mode());
      break;
    case AudioCompanionSettingsReceiver:
      title = i18n_noop("Receiver");
      subtitle = audio_companion_get_receiver_name(receiver_name, sizeof(receiver_name)) ?
                 receiver_name : i18n_noop("Not Paired");
      break;
    case AudioCompanionSettingsCount:
      break;
  }

  PBL_ASSERTN(title && subtitle);
  menu_cell_basic_draw(ctx, cell_layer, i18n_get(title, data), i18n_get(subtitle, data), NULL);
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsAudioCompanionData *data = (SettingsAudioCompanionData *)context;
  switch ((AudioCompanionSettingsItem)row) {
    case AudioCompanionSettingsToggle:
      audio_companion_set_enabled(!audio_companion_is_enabled());
      settings_menu_reload_data(SettingsMenuItemAudioCompanion);
      settings_menu_mark_dirty(SettingsMenuItemAudioCompanion);
      break;
    case AudioCompanionSettingsStatus:
      prv_show_diagnostics(data);
      break;
    case AudioCompanionSettingsSkipSilence:
      audio_companion_set_silence_mode(
          prv_next_silence_mode(audio_companion_get_silence_mode()));
      settings_menu_reload_data(SettingsMenuItemAudioCompanion);
      settings_menu_mark_dirty(SettingsMenuItemAudioCompanion);
      break;
    case AudioCompanionSettingsReceiver: {
      char receiver_name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];
      if (audio_companion_get_receiver_name(receiver_name, sizeof(receiver_name))) {
        prv_show_forget_receiver_confirm(data);
      }
      break;
    }
    case AudioCompanionSettingsCount:
      break;
  }
}

static Window *prv_init(void) {
  SettingsAudioCompanionData *data = app_zalloc_check(sizeof(*data));
  data->callbacks = (SettingsCallbacks) {
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
    .appear = prv_appear_cb,
    .hide = prv_hide_cb,
  };
  return settings_window_create(SettingsMenuItemAudioCompanion, &data->callbacks);
}

const SettingsModuleMetadata *settings_audio_companion_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("Audio Companion"),
    .init = prv_init,
  };
  return &s_module_info;
}

#endif
