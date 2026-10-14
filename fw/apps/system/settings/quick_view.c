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
#include "pbl/services/notifications/alerts_preferences.h"
#include "pbl/services/notifications/alerts_preferences_private.h"
#include "pbl/services/notifications/alerts_private.h"
#include "shell/prefs.h"
#include "system/passert.h"
#include "pbl/util/size.h"
#include "pbl/util/units.h"

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

// No "Disabled": whether a notification peek shows at all is up to Notifications > Show As and
// the muted-peek toggle, and the duration row is hidden when none can.
static const char *s_notif_labels[] = {
    i18n_noop("5 Seconds"),
    i18n_noop("15 Seconds"),
    i18n_noop("1 Minute"),
    i18n_noop("Until Dismissed"),
};

static const uint16_t s_notif_values[] = {
    5, 15, PBL_SEC_PER_MIN, QUICK_VIEW_NOTIF_SECONDS_PERSISTENT,
};

static int prv_notif_seconds_to_index(uint16_t seconds) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(s_notif_values); i++) {
    if (s_notif_values[i] == seconds) {
      return (int)i;
    }
  }
  return 1; // 15 Seconds, the default
}

static void prv_notif_menu_select(OptionMenu *option_menu, int selection, void *context) {
  quick_view_prefs_set_notif_seconds(s_notif_values[selection]);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

bool settings_quick_view_notif_show_as_visible(void) {
  return (alerts_get_mask() & AlertMobile);
}

QuickViewNotifStyle settings_quick_view_notif_effective_style(void) {
  if (quick_view_prefs_get_notif_seconds() == 0) {
    return QuickViewNotifStyle_FullScreen;
  }
  const QuickViewNotifStyle style = quick_view_prefs_get_notif_style();
  return (style < QuickViewNotifStyleCount) ? style : QuickViewNotifStyle_FullScreenPeekIfMissed;
}

bool settings_quick_view_notif_missed_visible(void) {
  return settings_quick_view_notif_show_as_visible() &&
         (settings_quick_view_notif_effective_style() != QuickViewNotifStyle_Peek) &&
         (alerts_get_notification_window_timeout_ms() != NOTIF_WINDOW_TIMEOUT_INFINITE);
}

void settings_quick_view_notif_missed_toggle(void) {
  const bool enable =
      (settings_quick_view_notif_effective_style() != QuickViewNotifStyle_FullScreenPeekIfMissed);
  if (enable && (quick_view_prefs_get_notif_seconds() == 0)) {
    quick_view_prefs_set_notif_seconds(QUICK_VIEW_NOTIF_SECONDS_DEFAULT);
  }
  quick_view_prefs_set_notif_style(enable ? QuickViewNotifStyle_FullScreenPeekIfMissed
                                          : QuickViewNotifStyle_FullScreen);
}

bool settings_quick_view_notif_muted_visible(void) {
  return !settings_quick_view_notif_show_as_visible() ||
         (alerts_preferences_dnd_get_show_notifications() == DndNotificationModeHide);
}

bool settings_quick_view_notif_seconds_visible(void) {
  if (quick_view_prefs_get_notif_seconds() == 0) {
    return false; // Off in a stored pref from before the duration lost its Disabled option
  }
  const QuickViewNotifStyle style = settings_quick_view_notif_effective_style();
  const bool peek_instead = settings_quick_view_notif_show_as_visible() &&
                            (style == QuickViewNotifStyle_Peek);
  const bool peek_if_missed = settings_quick_view_notif_missed_visible() &&
                              (style == QuickViewNotifStyle_FullScreenPeekIfMissed);
  const bool muted_peek = settings_quick_view_notif_muted_visible() &&
                          quick_view_prefs_get_notif_muted_enabled();
  return peek_instead || peek_if_missed || muted_peek;
}

void settings_quick_view_notif_muted_toggle(void) {
  const bool enable = !quick_view_prefs_get_notif_muted_enabled();
  if (enable && (quick_view_prefs_get_notif_seconds() == 0)) {
    quick_view_prefs_set_notif_seconds(QUICK_VIEW_NOTIF_SECONDS_DEFAULT);
  }
  quick_view_prefs_set_notif_muted_enabled(enable);
}

const char *settings_quick_view_notif_seconds_label(void) {
  return s_notif_labels[prv_notif_seconds_to_index(quick_view_prefs_get_notif_seconds())];
}

void settings_quick_view_notif_seconds_menu_push(const char *i18n_title_key, void *i18n_owner) {
  const int index = prv_notif_seconds_to_index(quick_view_prefs_get_notif_seconds());
  const OptionMenuCallbacks callbacks = {
      .select = prv_notif_menu_select,
  };
  settings_option_menu_push(
      i18n_title_key, OptionMenuContentType_SingleLine, index, &callbacks,
      ARRAY_LENGTH(s_notif_labels), true /* icons_enabled */, s_notif_labels, i18n_owner);
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

//! The rows on screen: the notification duration only when a notification peek can show.
static uint16_t prv_get_items(enum SettingsQuickViewItem items_out[NumSettingsQuickViewItems]) {
  uint16_t num_items = 0;
  for (int item = 0; item < NumSettingsQuickViewItems; item++) {
    if ((item == SettingsQuickViewNotifications) && !settings_quick_view_notif_seconds_visible()) {
      continue;
    }
    items_out[num_items++] = item;
  }
  return num_items;
}

static enum SettingsQuickViewItem prv_item_for_row(uint16_t row) {
  enum SettingsQuickViewItem items[NumSettingsQuickViewItems];
  const uint16_t num_items = prv_get_items(items);
  return (row < num_items) ? items[row] : NumSettingsQuickViewItems;
}

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

  switch (prv_item_for_row(row)) {
    case SettingsQuickViewMusic: {
      title = i18n_noop("Music");
      const QuickViewMusicMode mode = quick_view_prefs_get_music_mode();
      subtitle = (mode < QuickViewMusicModeCount) ? s_music_mode_labels[mode]
                                                  : i18n_noop("Unknown");
      break;
    }
    case SettingsQuickViewNotifications: {
      title = i18n_noop("Notifications");
      subtitle = settings_quick_view_notif_seconds_label();
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

  switch (prv_item_for_row(row)) {
    case SettingsQuickViewMusic:
      prv_music_mode_menu_push(data);
      break;
    case SettingsQuickViewNotifications:
      settings_quick_view_notif_seconds_menu_push(i18n_noop("Notifications"), data);
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
  enum SettingsQuickViewItem items[NumSettingsQuickViewItems];
  return prv_get_items(items);
}

static void prv_appear_cb(SettingsCallbacks *context) {
  // Settings on other pages decide which rows show
  settings_menu_reload_rows(SettingsMenuItemQuickView);
}

static Window *prv_init(void) {
  SettingsQuickViewData *data = app_malloc_check(sizeof(*data));
  *data = (SettingsQuickViewData){};

  data->callbacks = (SettingsCallbacks) {
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
    .appear = prv_appear_cb,
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
