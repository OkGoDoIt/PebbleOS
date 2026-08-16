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
  AudioCompanionSettingsRestarts,
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

static void prv_push_detail_dialog(const char *header, const char *text) {
  ExpandableDialog *dialog = expandable_dialog_create_with_params(
      "Audio Diagnostics", RESOURCE_ID_AUDIO_CASSETTE_LARGE, text, GColorBlack, GColorWhite,
      NULL, RESOURCE_ID_ACTION_BAR_ICON_CHECK, prv_diagnostics_close);
  expandable_dialog_show_action_bar(dialog, true);
  expandable_dialog_set_header(dialog, i18n_get(header, dialog));
  app_expandable_dialog_push(dialog);
}

//! The dialog copies whatever it is handed (dialog_set_text() allocates to fit) and the expandable
//! dialog scrolls, so DIALOG_MAX_MESSAGE_LEN is only a convention for callers -- these bodies get
//! room to say the whole thing rather than being cut off mid-word.
#define DETAIL_TEXT_MAX_LEN (512)

static void prv_show_diagnostics(SettingsAudioCompanionData *data) {
  AudioCompanionDiagnostics diag;
  audio_companion_get_diagnostics(&diag);

  char *text = app_zalloc_check(DETAIL_TEXT_MAX_LEN);
  sniprintf(text, DETAIL_TEXT_MAX_LEN,
            "State: %s\nCaptured: %" PRIu32 "\nSent: %" PRIu32 "\nBuffered: %" PRIu32
            " B\nPeak: %" PRIu32 " B\nDropped: %" PRIu32 "\nGaps: %" PRIu32
            "\nBackpressure: %" PRIu32 "\nMic conflicts: %" PRIu32 "\nFree heap: %" PRIu32 " B",
            i18n_get(prv_state_name(diag.state), data), diag.captured_frames, diag.sent_frames,
            diag.spool_bytes, diag.spool_high_water_bytes, diag.dropped_overflow_frames,
            diag.gap_records, diag.send_backpressure_events, diag.mic_conflicts,
            diag.kernel_heap_free_bytes);
  prv_push_detail_dialog(i18n_noop("Diagnostics"), text);
  app_free(text);
}

//! Compact "how long did that session last" for the restart list.
static void prv_format_duration(uint32_t seconds, char *buf, size_t buf_size) {
  if (seconds == 0) {
    sniprintf(buf, buf_size, "?");
  } else if (seconds < 60) {
    sniprintf(buf, buf_size, "%" PRIu32 "s", seconds);
  } else if (seconds < 60 * 60) {
    sniprintf(buf, buf_size, "%" PRIu32 "m", seconds / 60);
  } else if (seconds < 24 * 60 * 60) {
    sniprintf(buf, buf_size, "%" PRIu32 "h", seconds / (60 * 60));
  } else {
    sniprintf(buf, buf_size, "%" PRIu32 "d", seconds / (24 * 60 * 60));
  }
}

//! Reboot flight recorder. Names the fault class *and* the task that stalled, and prints the raw
//! PC/LR/callback so an address can be symbolized against the firmware ELF -- "watchdog" alone
//! only says the watch hung, not what hung it.
static void prv_show_restarts(SettingsAudioCompanionData *data) {
  AudioCompanionRebootTrace trace;
  audio_companion_get_reboot_trace(&trace);
  const AudioCompanionRebootTraceEntry *last = audio_companion_reboot_trace_newest(&trace);

  char *text = app_zalloc_check(DETAIL_TEXT_MAX_LEN);
  if (!last) {
    sniprintf(text, DETAIL_TEXT_MAX_LEN, "%s", i18n_get("No restarts recorded yet.", data));
    prv_push_detail_dialog(i18n_noop("Restarts"), text);
    app_free(text);
    return;
  }

  int written = sniprintf(text, DETAIL_TEXT_MAX_LEN, "Total: %" PRIu16 "\nFaults: %" PRIu16,
                          trace.total_reboots, trace.total_error_reboots);
  if (trace.consecutive_fault_boots > 0 && written > 0 && written < DETAIL_TEXT_MAX_LEN) {
    written += sniprintf(text + written, DETAIL_TEXT_MAX_LEN - written, "\nIn a row: %" PRIu8,
                         trace.consecutive_fault_boots);
  }

  // Lead with the sticky last fault: the ring is evicted by ordinary restarts, so after a crash
  // that forced a firmware reload this is often the only place the crash still exists.
  const AudioCompanionRebootTraceEntry *fault = audio_companion_reboot_trace_last_fault(&trace);
  if (fault && written > 0 && written < DETAIL_TEXT_MAX_LEN) {
    char ran_for[12];
    prv_format_duration(trace.last_fault_session_seconds, ran_for, sizeof(ran_for));
    written += sniprintf(text + written, DETAIL_TEXT_MAX_LEN - written,
                         "\n\nLast fault: %s\nAfter: %s up\nAudio was %s",
                         audio_companion_reboot_trace_reason_name(fault->reason_code), ran_for,
                         (fault->flags & AudioCompanionRebootTraceFlagEnabled) ? "on" : "off");

    const char *stuck = audio_companion_reboot_trace_stuck_task_name(fault);
    if (stuck && written > 0 && written < DETAIL_TEXT_MAX_LEN) {
      written += sniprintf(text + written, DETAIL_TEXT_MAX_LEN - written, "\nStuck: %s", stuck);
    }
    if (fault->fault_pc && written > 0 && written < DETAIL_TEXT_MAX_LEN) {
      written += sniprintf(text + written, DETAIL_TEXT_MAX_LEN - written,
                           "\nPC %08" PRIx32 "\nLR %08" PRIx32, fault->fault_pc, fault->fault_lr);
    }
    if (fault->fault_extra && written > 0 && written < DETAIL_TEXT_MAX_LEN) {
      written += sniprintf(text + written, DETAIL_TEXT_MAX_LEN - written, "\nCB %08" PRIx32,
                           fault->fault_extra);
    }
  }

  // Then the recent history, newest first. Each line reports how long that session ran, which is
  // what separates a one-off after a day of uptime from a boot loop.
  if (written > 0 && written < DETAIL_TEXT_MAX_LEN) {
    written += sniprintf(text + written, DETAIL_TEXT_MAX_LEN - written, "\n\nRecent:");
  }
  for (uint8_t i = 0; i < AUDIO_COMPANION_REBOOT_TRACE_ENTRIES; i++) {
    const AudioCompanionRebootTraceEntry *entry = audio_companion_reboot_trace_at(&trace, i);
    if (!entry || written <= 0 || written >= DETAIL_TEXT_MAX_LEN) {
      break;
    }
    const AudioCompanionRebootTraceEntry *older = audio_companion_reboot_trace_at(&trace, i + 1);
    char ran_for[12];
    prv_format_duration(
        (older && entry->boot_wall_time > older->boot_wall_time)
            ? (entry->boot_wall_time - older->boot_wall_time) : 0,
        ran_for, sizeof(ran_for));
    const char *stuck = audio_companion_reboot_trace_stuck_task_name(entry);
    written += sniprintf(text + written, DETAIL_TEXT_MAX_LEN - written, "\n%s %s%s%s",
                         audio_companion_reboot_trace_reason_name(entry->reason_code), ran_for,
                         stuck ? " " : "", stuck ? stuck : "");
  }

  prv_push_detail_dialog(i18n_noop("Restarts"), text);
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
  char restarts_summary[24] = {0};

  switch ((AudioCompanionSettingsItem)row) {
    case AudioCompanionSettingsToggle:
      title = i18n_noop("Background Audio");
      subtitle = audio_companion_is_enabled() ? i18n_noop("On") : i18n_noop("Off");
      break;
    case AudioCompanionSettingsStatus:
      title = i18n_noop("Status");
      subtitle = prv_state_name(audio_companion_get_state());
      break;
    case AudioCompanionSettingsRestarts: {
      title = i18n_noop("Restarts");
      AudioCompanionRebootTrace trace;
      audio_companion_get_reboot_trace(&trace);
      const AudioCompanionRebootTraceEntry *last = audio_companion_reboot_trace_newest(&trace);
      const AudioCompanionRebootTraceEntry *fault =
          audio_companion_reboot_trace_last_fault(&trace);
      if (!last) {
        subtitle = i18n_noop("None");
      } else if (fault) {
        // Surface the fault, not the benign restart that happened to come after it -- reloading
        // firmware to recover from a crash would otherwise hide the crash behind "FW Update".
        sniprintf(restarts_summary, sizeof(restarts_summary), "%s (%" PRIu16 " faults)",
                  audio_companion_reboot_trace_reason_name(fault->reason_code),
                  trace.total_error_reboots);
        subtitle = restarts_summary;
      } else {
        sniprintf(restarts_summary, sizeof(restarts_summary), "%s (%" PRIu16 ")",
                  audio_companion_reboot_trace_reason_name(last->reason_code),
                  trace.total_reboots);
        subtitle = restarts_summary;
      }
      break;
    }
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
    case AudioCompanionSettingsRestarts:
      prv_show_restarts(data);
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
