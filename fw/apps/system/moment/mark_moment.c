/* SPDX-License-Identifier: Apache-2.0 */

#include "mark_moment.h"

#ifdef CONFIG_SERVICE_AUDIO_COMPANION

#include <applib/app.h>
#include <applib/app_exit_reason.h>
#include <applib/ui/dialogs/dialog.h>
#include <applib/ui/dialogs/simple_dialog.h>
#include <applib/ui/vibes.h>
#include <pbl/services/audio_companion.h>
#include <resource/resource_ids.auto.h>

//! Long enough to read "Marked" and the line under it on the way back to the watchface.
#define MARK_MOMENT_DIALOG_MS (1800)

static const char *prv_text(AudioCompanionMarkResult result) {
  switch (result) {
    case AudioCompanionMarkRecorded:
      return "Moment saved\nFind it in Pebble Audio";
    case AudioCompanionMarkNotRecording:
      return "Marked\nNot recording right now";
    case AudioCompanionMarkQueueFull:
      return "Not marked\nOpen Pebble Audio to sync saved moments";
    case AudioCompanionMarkSaveFailed:
      return "Couldn't save moment\nPlease try again";
    case AudioCompanionMarkNoReceiver:
    default:
      return "Set up Pebble Audio on your phone first";
  }
}

static void prv_main(void) {
  // Marked the moment the button was held, before anything is drawn.
  const AudioCompanionMarkResult result = audio_companion_mark_moment();
  const bool marked =
      result == AudioCompanionMarkRecorded || result == AudioCompanionMarkNotRecording;
  if (marked) {
    vibes_short_pulse();
    app_exit_reason_set(APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY);
  } else {
    vibes_double_pulse();
  }

  SimpleDialog *simple_dialog = simple_dialog_create("MarkMoment");
  Dialog *dialog = simple_dialog_get_dialog(simple_dialog);
  dialog_set_text(dialog, prv_text(result));
  dialog_set_icon(dialog, marked ? RESOURCE_ID_GENERIC_PIN_LARGE
                                 : RESOURCE_ID_GENERIC_WARNING_LARGE);
  dialog_set_background_color(dialog, PBL_IF_COLOR_ELSE(marked ? GColorMediumAquamarine
                                                               : GColorMelon,
                                                        GColorWhite));
  dialog_set_timeout(dialog, marked ? MARK_MOMENT_DIALOG_MS : DIALOG_TIMEOUT_INFINITE);
  app_simple_dialog_push(simple_dialog);
  app_event_loop();
}

const PebbleProcessMd *mark_moment_get_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common =
        {
          .main_func = &prv_main,
          // UUID: 5d1f7c2e-8a43-4b9e-b6d0-2f4e81a3c957
          .uuid = {0x5d, 0x1f, 0x7c, 0x2e, 0x8a, 0x43, 0x4b, 0x9e, 0xb6, 0xd0, 0x2f, 0x4e, 0x81,
                   0xa3, 0xc9, 0x57},
          .visibility = ProcessVisibilityQuickLaunch,
        },
    .name = "Mark Moment",
    .icon_resource_id = RESOURCE_ID_GENERIC_PIN_TINY,
  };
  return (const PebbleProcessMd *)&s_app_info;
}

#endif  // CONFIG_SERVICE_AUDIO_COMPANION
