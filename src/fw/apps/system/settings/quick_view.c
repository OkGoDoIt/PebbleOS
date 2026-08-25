/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#ifdef CONFIG_SERVICE_PEEK_WIDGETS

#include "quick_view.h"
#include "menu.h"
#include "option_menu.h"
#include "window.h"

#include "applib/ui/option_menu_window.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "pbl/services/i18n/i18n.h"
#include "shell/prefs.h"
#include "system/passert.h"
#include "pbl/util/size.h"

typedef struct SettingsQuickViewData {
  SettingsCallbacks callbacks;
} SettingsQuickViewData;

enum SettingsQuickViewItem {
  SettingsQuickViewMusic,
  SettingsQuickViewNotifications,
  SettingsQuickViewAppWidgets,
  SettingsQuickViewButton,
  NumSettingsQuickViewItems
};

// Music widget option menu
/////////////////////////////

static const char *s_music_mode_labels[QuickViewMusicModeCount] = {
    [QuickViewMusicMode_Disabled] = i18n_noop("Disabled"),
    [QuickViewMusicMode_WhilePlaying] = i18n_noop("While Playing"),
    [QuickViewMusicMode_TrackStart] = i18n_noop("Start of Track"),
};

static void prv_music_mode_menu_select(OptionMenu *option_menu, int selection, void *context) {
  quick_view_prefs_set_music_mode((QuickViewMusicMode)selection);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_music_mode_menu_push(SettingsQuickViewData *data) {
  const int index = (int)quick_view_prefs_get_music_mode();
  const OptionMenuCallbacks callbacks = {
      .select = prv_music_mode_menu_select,
  };
  const char *title = i18n_noop("Music");
  settings_option_menu_push(
      title, OptionMenuContentType_SingleLine, index, &callbacks,
      ARRAY_LENGTH(s_music_mode_labels), true /* icons_enabled */, s_music_mode_labels, data);
}

// Notifications widget option menu
/////////////////////////////

static const char *s_notif_labels[] = {
    i18n_noop("Disabled"),
    i18n_noop("5 Seconds"),
    i18n_noop("15 Seconds"),
    i18n_noop("1 Minute"),
    i18n_noop("Until Dismissed"),
};

static const uint16_t s_notif_values[] = {
    0, 5, 15, SECONDS_PER_MINUTE, QUICK_VIEW_NOTIF_SECONDS_PERSISTENT,
};

static int prv_notif_seconds_to_index(uint16_t seconds) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(s_notif_values); i++) {
    if (s_notif_values[i] == seconds) {
      return (int)i;
    }
  }
  return 2; // 15 Seconds, the default
}

static void prv_notif_menu_select(OptionMenu *option_menu, int selection, void *context) {
  quick_view_prefs_set_notif_seconds(s_notif_values[selection]);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_notif_menu_push(SettingsQuickViewData *data) {
  const int index = prv_notif_seconds_to_index(quick_view_prefs_get_notif_seconds());
  const OptionMenuCallbacks callbacks = {
      .select = prv_notif_menu_select,
  };
  const char *title = i18n_noop("Notifications");
  settings_option_menu_push(
      title, OptionMenuContentType_SingleLine, index, &callbacks,
      ARRAY_LENGTH(s_notif_labels), true /* icons_enabled */, s_notif_labels, data);
}

// Button shortcut option menu
/////////////////////////////

static const char *s_button_mode_labels[QuickViewButtonModeCount] = {
    [QuickViewButtonMode_Disabled] = i18n_noop("Disabled"),
    [QuickViewButtonMode_Press] = i18n_noop("Press"),
    [QuickViewButtonMode_DoublePress] = i18n_noop("Double Press"),
    [QuickViewButtonMode_Hold] = i18n_noop("Hold"),
};

static void prv_button_mode_menu_select(OptionMenu *option_menu, int selection, void *context) {
  quick_view_prefs_set_button_mode((QuickViewButtonMode)selection);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_button_mode_menu_push(SettingsQuickViewData *data) {
  const int index = (int)quick_view_prefs_get_button_mode();
  const OptionMenuCallbacks callbacks = {
      .select = prv_button_mode_menu_select,
  };
  const char *title = i18n_noop("Open With Down");
  settings_option_menu_push(
      title, OptionMenuContentType_SingleLine, index, &callbacks,
      ARRAY_LENGTH(s_button_mode_labels), true /* icons_enabled */, s_button_mode_labels, data);
}

// Menu Callbacks
/////////////////////////////

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsQuickViewData *data = (SettingsQuickViewData *)context;

  i18n_free_all(data);
  app_free(data);
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx,
                            const Layer *cell_layer, uint16_t row, bool selected) {
  SettingsQuickViewData *data = (SettingsQuickViewData *)context;

  const char *title = NULL;
  const char *subtitle = NULL;

  switch (row) {
    case SettingsQuickViewMusic: {
      title = i18n_noop("Music");
      const QuickViewMusicMode mode = quick_view_prefs_get_music_mode();
      subtitle = (mode < QuickViewMusicModeCount) ? s_music_mode_labels[mode]
                                                  : i18n_noop("Unknown");
      break;
    }
    case SettingsQuickViewNotifications: {
      title = i18n_noop("Notifications");
      subtitle = s_notif_labels[prv_notif_seconds_to_index(quick_view_prefs_get_notif_seconds())];
      break;
    }
    case SettingsQuickViewAppWidgets: {
      title = i18n_noop("App Widgets");
      subtitle = quick_view_prefs_get_apps_enabled() ? i18n_noop("On") : i18n_noop("Off");
      break;
    }
    case SettingsQuickViewButton: {
      title = i18n_noop("Open With Down");
      const QuickViewButtonMode mode = quick_view_prefs_get_button_mode();
      subtitle = (mode < QuickViewButtonModeCount) ? s_button_mode_labels[mode]
                                                   : i18n_noop("Unknown");
      break;
    }
    default:
      WTF;
  }
  menu_cell_basic_draw(ctx, cell_layer, i18n_get(title, data), i18n_get(subtitle, data), NULL);
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsQuickViewData *data = (SettingsQuickViewData *)context;

  switch (row) {
    case SettingsQuickViewMusic:
      prv_music_mode_menu_push(data);
      break;
    case SettingsQuickViewNotifications:
      prv_notif_menu_push(data);
      break;
    case SettingsQuickViewAppWidgets:
      quick_view_prefs_set_apps_enabled(!quick_view_prefs_get_apps_enabled());
      break;
    case SettingsQuickViewButton:
      prv_button_mode_menu_push(data);
      break;
    default:
      WTF;
  }
  settings_menu_reload_data(SettingsMenuItemQuickView);
  settings_menu_mark_dirty(SettingsMenuItemQuickView);
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  return NumSettingsQuickViewItems;
}

static Window *prv_init(void) {
  SettingsQuickViewData *data = app_malloc_check(sizeof(*data));
  *data = (SettingsQuickViewData){};

  data->callbacks = (SettingsCallbacks) {
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
  };

  return settings_window_create(SettingsMenuItemQuickView, &data->callbacks);
}

const SettingsModuleMetadata *settings_quick_view_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("Quick View"),
    .init = prv_init,
  };

  return &s_module_info;
}

#endif // CONFIG_SERVICE_PEEK_WIDGETS
