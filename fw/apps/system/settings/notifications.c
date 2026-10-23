/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "menu.h"
#include "notifications_private.h"
#include "option_menu.h"
#include "quick_view.h"
#include "window.h"

#include <pbl/kernel/assert.h>
#include <pbl/kernel/fatal.h>
#include <pbl/services/i18n/i18n.h>
#include <pbl/services/notifications/alerts_preferences_private.h>
#include <pbl/services/notifications/alerts_private.h>
#include <pbl/util/size.h>
#include <pbl/util/units.h>

#include <applib/event_service_client.h>
#include <applib/ui/action_menu_window_private.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/option_menu_window.h>
#include <applib/ui/ui.h>
#include <kernel/pbl_malloc.h>
#include <shell/prefs.h>

// Offset between vibe intensity menu item index and vibe intensity enum values
#define INTENSITY_ROW_OFFSET 1

// The settings this page can show. Which ones show, and in what order, depends on other
// settings: see prv_get_items().
enum NotificationsItem {
  NotificationsItemFilter,
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  NotificationsItemShowAs,
#endif
  NotificationsItemWindowTimeout,
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  NotificationsItemPeekIfMissed,
  NotificationsItemPeekWhenMuted,
  NotificationsItemPeekDuration,
#endif
  NotificationsItemTextSize,
#if PBL_BW
  NotificationsItemDesignStyle,
#endif
  NotificationsItemVibeDelay,
  NotificationsItemBacklight,
  NotificationsItemGroupBySender,
  NotificationsItemStatusBarStyle,
  NotificationsItem_Count,
};

typedef struct {
  SettingsCallbacks callbacks;
  EventServiceInfo battery_connection_event_info;
  //! The setting last selected, so the selection follows it when rows come and go.
  enum NotificationsItem selected_item;
} SettingsNotificationsData;

// Filter Alerts
//////////////////////////

#define NUM_ALERT_MODES_IN_LIST 3

// These aren't all of the values of AlertMask, so to add extra ones you have to update both of
// these arrays

static const AlertMask s_alert_mode_values[NUM_ALERT_MODES_IN_LIST] = {
  AlertMaskAllOn,
  AlertMaskPhoneCalls,
  AlertMaskAllOff,
};

static const char *s_alert_mode_labels[NUM_ALERT_MODES_IN_LIST] = {
  i18n_noop("Allow All Notifications"),
  i18n_noop("Allow Phone Calls Only"),
  i18n_noop("Mute All Notifications"),
};

static const char *prv_alert_mask_to_label(AlertMask mask) {
  for (uint32_t i = 0; i < NUM_ALERT_MODES_IN_LIST; i++) {
    if (s_alert_mode_values[i] == mask) {
      return s_alert_mode_labels[i];
    }
  }
  return "???";
}

static void prv_filter_menu_select(OptionMenu *option_menu, int selection, void *context) {
  alerts_set_mask(s_alert_mode_values[selection]);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_filter_menu_push(SettingsNotificationsData *data) {
  AlertMask mask = alerts_get_mask();
  size_t cycle_len = ARRAY_LENGTH(s_alert_mode_values);
  size_t index = 0;
  for (size_t i = 0; i < cycle_len; i++) {
    if (s_alert_mode_values[i] == mask) {
      index = i;
      break;
    }
  }
  const OptionMenuCallbacks callbacks = {
    .select = prv_filter_menu_select,
  };
  /// The option in the Settings app for which notifications alert. The phone app calls this
  /// "Notification Filter"; it is not the per-app list.
  const char *title = i18n_noop("Alerts");
  settings_option_menu_push(title, OptionMenuContentType_DoubleLine, index, &callbacks, cycle_len,
                            true /* icons_enabled */, s_alert_mode_labels, data);
}

#ifdef CONFIG_SERVICE_PEEK_WIDGETS
// Show As
////////////////////////

static const char *s_show_as_labels[] = {
  /// Show As: the full-screen notification popup
  i18n_noop("Full Screen"),
  /// Show As: a vibration and the Quick View peek on the watchface, no popup
  i18n_noop("Quick View Peek"),
};

static bool prv_show_as_is_peek(void) {
  return (settings_quick_view_notif_effective_style() == QuickViewNotifStyle_Peek);
}

static void prv_show_as_menu_select(OptionMenu *option_menu, int selection, void *context) {
  const bool peek = (selection == 1);
  if (peek != prv_show_as_is_peek()) {
    if (peek && (quick_view_prefs_get_notif_seconds() == 0)) {
      quick_view_prefs_set_notif_seconds(QUICK_VIEW_NOTIF_SECONDS_DEFAULT);
    }
    // Back to full screen restores the default of peeking when a popup is missed
    quick_view_prefs_set_notif_style(peek ? QuickViewNotifStyle_Peek
                                          : QuickViewNotifStyle_FullScreenPeekIfMissed);
  }
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_show_as_menu_push(SettingsNotificationsData *data) {
  const OptionMenuCallbacks callbacks = {
    .select = prv_show_as_menu_select,
  };
  /// The option in the Settings app for how notifications that alert appear
  const char *title = i18n_noop("Show As");
  settings_option_menu_push(title, OptionMenuContentType_SingleLine, prv_show_as_is_peek() ? 1 : 0,
                            &callbacks, ARRAY_LENGTH(s_show_as_labels), true /* icons_enabled */,
                            s_show_as_labels, data);
}

static const char *prv_peek_when_muted_title(void) {
  if (!settings_quick_view_notif_show_as_visible()) {
    /// Alerts mutes notifications: whether they still show as a silent Quick View peek
    return i18n_noop("Silent Peek");
  }
  /// Quiet Time hides notifications: whether they still show as a silent Quick View peek
  return i18n_noop("Peek During Quiet Time");
}
#endif

// Text Size
////////////////////////

enum {
  NotificationsTextSizeSystem = SettingsContentSizeCount,
  NotificationsTextSizeCount,
};

static const char *s_text_size_names[NotificationsTextSizeCount] = {
  [SettingsContentSize_Small] = i18n_noop("Smaller"),
  [SettingsContentSize_Default] = i18n_ctx_noop("TextSize", "Default"),
  [SettingsContentSize_Large] = i18n_noop("Larger"),
  /// Notification text size option that follows the system Text Size setting
  [NotificationsTextSizeSystem] = i18n_noop("Same as System"),
};

static int prv_text_size_get_selection_index(void) {
  const PreferredContentSize size = alerts_preferences_get_notification_content_size();
  return (size == NotificationContentSizeSystem) ? NotificationsTextSizeSystem
                                                 : settings_content_size_from_preferred_size(size);
}

static void prv_text_size_menu_select(OptionMenu *option_menu, int selection, void *context) {
  alerts_preferences_set_notification_content_size(
      (selection == NotificationsTextSizeSystem)
          ? NotificationContentSizeSystem
          : settings_content_size_to_preferred_size(selection));
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_text_size_menu_push(SettingsNotificationsData *data) {
  const OptionMenuCallbacks callbacks = {
    .select = prv_text_size_menu_select,
  };
  /// The option in the Settings app for choosing the text size of notifications.
  const char *title = i18n_noop("Text Size");
  settings_option_menu_push(
      title, OptionMenuContentType_SingleLine, prv_text_size_get_selection_index(), &callbacks,
      NotificationsTextSizeCount, true /* icons_enabled */, s_text_size_names, data);
}

// Window Timeout
////////////////////////

// NOTE: Keep the following two arrays in sync and with the same size.
static const uint32_t s_window_timeouts_ms[] = {
  NOTIF_WINDOW_TIMEOUT_MIN,     30 * PBL_MSEC_PER_SEC, 1 * PBL_MSEC_PER_MIN,
  NOTIF_WINDOW_TIMEOUT_DEFAULT, 10 * PBL_MSEC_PER_MIN, NOTIF_WINDOW_TIMEOUT_INFINITE
};

static const char *s_window_timeouts_labels[] = {
  /// 15 Second Notification Window Timeout
  i18n_noop("15 Seconds"),
  /// 30 Second Notification Window Timeout
  i18n_noop("30 Seconds"),
  /// 1 Minute Notification Window Timeout
  i18n_noop("1 Minute"),
  /// 3 Minute Notification Window Timeout
  i18n_noop("3 Minutes"),
  /// 10 Minute Notification Window Timeout
  i18n_noop("10 Minutes"),
  /// No Notification Window Timeout
  i18n_noop("None"),
};

static_assert(ARRAY_LENGTH(s_window_timeouts_ms) == ARRAY_LENGTH(s_window_timeouts_labels), "");

static int prv_window_timeout_get_selection_index(void) {
  const int DEFAULT_IDX = 3;
  // Double check no one has fudged with the order and the fallback/default
  PBL_ASSERTN(s_window_timeouts_ms[DEFAULT_IDX] == NOTIF_WINDOW_TIMEOUT_DEFAULT);

  const uint32_t timeout_ms = alerts_preferences_get_notification_window_timeout_ms();
  for (size_t i = 0; i < ARRAY_LENGTH(s_window_timeouts_ms); i++) {
    if (s_window_timeouts_ms[i] == timeout_ms) {
      return i;
    }
  }
  // Should never happen (only should happen if we remove a timeout and don't migrate the user
  // to a new setting automatically
  return DEFAULT_IDX;
}

static void prv_window_timeout_menu_select(OptionMenu *option_menu, int selection, void *context) {
  alerts_preferences_set_notification_window_timeout_ms(s_window_timeouts_ms[selection]);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static const char *prv_window_timeout_title(void);

static void prv_window_timeout_menu_push(SettingsNotificationsData *data) {
  const int index = prv_window_timeout_get_selection_index();
  const OptionMenuCallbacks callbacks = {
    .select = prv_window_timeout_menu_select,
  };
  /// Status bar title for the Notification Window Timeout settings screen
  const char *title = prv_window_timeout_title();
  settings_option_menu_push(title, OptionMenuContentType_SingleLine, index, &callbacks,
                            ARRAY_LENGTH(s_window_timeouts_labels), true /* icons_enabled */,
                            s_window_timeouts_labels, data);
}

// Design Style
////////////////////////

#if PBL_BW
static const char *s_design_style_labels[] = {
  /// Standard notification design option (default)
  i18n_noop("Classic"),
  /// Alternative notification design option
  i18n_noop("Flat Black"),
};

static int prv_design_style_get_selection_index(void) {
  return alerts_preferences_get_notification_alternative_design() ? 1 : 0;
}

static void prv_design_style_menu_select(OptionMenu *option_menu, int selection, void *context) {
  alerts_preferences_set_notification_alternative_design(selection == 1);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_design_style_menu_push(SettingsNotificationsData *data) {
  const int index = prv_design_style_get_selection_index();
  const OptionMenuCallbacks callbacks = {
    .select = prv_design_style_menu_select,
  };
  /// Status bar title for the Notification Design Style settings screen
  const char *title = i18n_noop("Banner Style");
  settings_option_menu_push(title, OptionMenuContentType_SingleLine, index, &callbacks,
                            ARRAY_LENGTH(s_design_style_labels), true /* icons_enabled */,
                            s_design_style_labels, data);
}
#endif /* PBL_BW */

// Vibe Delay
////////////////////////

static const char *s_vibe_delay_labels[] = {
  /// Vibrate at the beginning of notification animation (immediate)
  i18n_noop("Beginning"),
  /// Vibrate at the end of notification animation (delayed)
  i18n_noop("End"),
};

static int prv_vibe_delay_get_selection_index(void) {
  return alerts_preferences_get_notification_vibe_delay() ? 1 : 0;
}

static void prv_vibe_delay_menu_select(OptionMenu *option_menu, int selection, void *context) {
  alerts_preferences_set_notification_vibe_delay(selection == 1);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static const char *prv_vibe_delay_title(void);

static void prv_vibe_delay_menu_push(SettingsNotificationsData *data) {
  const int index = prv_vibe_delay_get_selection_index();
  const OptionMenuCallbacks callbacks = {
    .select = prv_vibe_delay_menu_select,
  };
  const char *title = prv_vibe_delay_title();
  settings_option_menu_push(title, OptionMenuContentType_SingleLine, index, &callbacks,
                            ARRAY_LENGTH(s_vibe_delay_labels), true /* icons_enabled */,
                            s_vibe_delay_labels, data);
}

// Status Bar Style
////////////////////////

static const char *s_status_bar_style_labels[] = {
  [NotificationStatusBarStyle_Default] = i18n_ctx_noop("StatusBar", "Default"),
  [NotificationStatusBarStyle_Bold] = i18n_noop("Bold"),
  [NotificationStatusBarStyle_LargeBold] = i18n_noop("Big & Bold"),
};

static_assert(ARRAY_LENGTH(s_status_bar_style_labels) == NotificationStatusBarStyleCount, "");

static int prv_status_bar_style_get_selection_index(void) {
  const NotificationStatusBarStyle style = alerts_preferences_get_notification_status_bar_style();
  return (style < NotificationStatusBarStyleCount) ? (int)style : 0;
}

static void prv_status_bar_style_menu_select(OptionMenu *option_menu, int selection,
                                             void *context) {
  alerts_preferences_set_notification_status_bar_style((NotificationStatusBarStyle)selection);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_status_bar_style_menu_push(SettingsNotificationsData *data) {
  const int index = prv_status_bar_style_get_selection_index();
  const OptionMenuCallbacks callbacks = {
    .select = prv_status_bar_style_menu_select,
  };
  /// Status bar title for the Notification Status Bar Style settings screen
  const char *title = i18n_noop("Clock Style");
  settings_option_menu_push(title, OptionMenuContentType_SingleLine, index, &callbacks,
                            ARRAY_LENGTH(s_status_bar_style_labels), true /* icons_enabled */,
                            s_status_bar_style_labels, data);
}

// Group by Sender
////////////////////////

static const char *s_notification_grouping_range_labels[] = {
  /// Disable notification grouping
  [NotificationGroupingRange_Never] = i18n_noop("Never"),
  /// Group notifications received within one day
  [NotificationGroupingRange_OneDay] = i18n_noop("1 Day"),
  /// Group notifications received within one week
  [NotificationGroupingRange_OneWeek] = i18n_noop("1 Week"),
  /// Group all notifications
  [NotificationGroupingRange_All] = i18n_noop("All"),
};

static_assert(ARRAY_LENGTH(s_notification_grouping_range_labels) == NotificationGroupingRangeCount,
              "");

static void prv_notification_grouping_range_menu_select(OptionMenu *option_menu, int selection,
                                                        void *context) {
  alerts_preferences_set_notification_grouping_range((NotificationGroupingRange)selection);
  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_notification_grouping_range_menu_push(SettingsNotificationsData *data) {
  const OptionMenuCallbacks callbacks = {
    .select = prv_notification_grouping_range_menu_select,
  };
  /// Title for the notification sender grouping settings screen
  const char *title = i18n_noop("Group by Sender");
  settings_option_menu_push(title, OptionMenuContentType_SingleLine,
                            alerts_preferences_get_notification_grouping_range(), &callbacks,
                            ARRAY_LENGTH(s_notification_grouping_range_labels),
                            true /* icons_enabled */, s_notification_grouping_range_labels, data);
}

// Menu Layer Callbacks
////////////////////////

//! Whether phone notifications can show as the full-screen popup. When they cannot, the
//! popup settings only affect calendar reminders, and are named and placed as such.
static bool prv_notifications_can_pop_up(void) {
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  return settings_quick_view_notif_show_as_visible() &&
         (settings_quick_view_notif_effective_style() != QuickViewNotifStyle_Peek);
#else
  return true;
#endif
}

static const char *prv_window_timeout_title(void) {
  if (!prv_notifications_can_pop_up()) {
    /// How long a calendar reminder popup stays up, when notifications never pop up
    return i18n_noop("Reminder Timeout");
  }
  /// How long the full-screen notification popup stays up
  return i18n_noop("Popup Timeout");
}

static const char *prv_vibe_delay_title(void) {
  if (!prv_notifications_can_pop_up()) {
    /// When a calendar reminder popup vibrates, when notifications never pop up
    return i18n_noop("Reminder Vibe Timing");
  }
  /// When the notification popup vibrates: as it starts appearing, or once it has
  return i18n_noop("Vibe Timing");
}

//! The rows on screen, in order: what alerts; how it appears, with that mode's own settings;
//! what muted notifications do; how notifications look; then, when notifications cannot pop
//! up, the popup settings that are left over for reminders.
static uint16_t prv_get_items(enum NotificationsItem items_out[NotificationsItem_Count]) {
  uint16_t n = 0;
  items_out[n++] = NotificationsItemFilter;
  const bool pop_ups = prv_notifications_can_pop_up();
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  if (settings_quick_view_notif_show_as_visible()) {
    items_out[n++] = NotificationsItemShowAs;
  }
#endif
  if (pop_ups) {
    items_out[n++] = NotificationsItemWindowTimeout;
    items_out[n++] = NotificationsItemVibeDelay;
  }
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  if (settings_quick_view_notif_missed_visible()) {
    items_out[n++] = NotificationsItemPeekIfMissed;
  }
  // The duration goes with the mode when the mode peeks, otherwise after the muted toggle
  const QuickViewNotifStyle style = settings_quick_view_notif_effective_style();
  const bool mode_peeks = settings_quick_view_notif_show_as_visible() &&
                          ((style == QuickViewNotifStyle_Peek) ||
                           (settings_quick_view_notif_missed_visible() &&
                            (style == QuickViewNotifStyle_FullScreenPeekIfMissed)));
  const bool duration = settings_quick_view_notif_seconds_visible();
  if (duration && mode_peeks) {
    items_out[n++] = NotificationsItemPeekDuration;
  }
  if (settings_quick_view_notif_muted_visible()) {
    items_out[n++] = NotificationsItemPeekWhenMuted;
  }
  if (duration && !mode_peeks) {
    items_out[n++] = NotificationsItemPeekDuration;
  }
#endif
  items_out[n++] = NotificationsItemBacklight;
  items_out[n++] = NotificationsItemTextSize;
#if PBL_BW
  items_out[n++] = NotificationsItemDesignStyle;
#endif
  items_out[n++] = NotificationsItemGroupBySender;
  items_out[n++] = NotificationsItemStatusBarStyle;
  if (!pop_ups) {
    items_out[n++] = NotificationsItemWindowTimeout;
    items_out[n++] = NotificationsItemVibeDelay;
  }
  return n;
}

static enum NotificationsItem prv_item_for_row(uint16_t row) {
  enum NotificationsItem items[NotificationsItem_Count];
  const uint16_t num_items = prv_get_items(items);
  return (row < num_items) ? items[row] : NotificationsItem_Count;
}

static int16_t prv_row_for_item(enum NotificationsItem item) {
  enum NotificationsItem items[NotificationsItem_Count];
  const uint16_t num_items = prv_get_items(items);
  for (uint16_t row = 0; row < num_items; row++) {
    if (items[row] == item) {
      return row;
    }
  }
  return -1; // Hidden now: keep the selection where it is
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  enum NotificationsItem items[NotificationsItem_Count];
  return prv_get_items(items);
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx, const Layer *cell_layer,
                            uint16_t row, bool selected) {
  SettingsNotificationsData *data = ((SettingsOptionMenuData *)context)->context;
  const char *subtitle = nullptr;
  const char *title = nullptr;

  switch (prv_item_for_row(row)) {
    case NotificationsItemFilter:
      title = i18n_noop("Alerts");
      subtitle = prv_alert_mask_to_label(alerts_get_mask());
      break;
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
    case NotificationsItemShowAs:
      title = i18n_noop("Show As");
      subtitle = s_show_as_labels[prv_show_as_is_peek() ? 1 : 0];
      break;
    case NotificationsItemPeekIfMissed:
      /// Whether a full-screen notification popup that times out untouched leaves the
      /// notification as a Quick View peek
      title = i18n_noop("Peek If Missed");
      subtitle = (settings_quick_view_notif_effective_style() ==
                  QuickViewNotifStyle_FullScreenPeekIfMissed)
                     ? i18n_noop("On")
                     : i18n_noop("Off");
      break;
    case NotificationsItemPeekDuration:
      /// How long a Quick View notification peek stays on the watchface
      title = i18n_noop("Peek Duration");
      subtitle = settings_quick_view_notif_seconds_label();
      break;
    case NotificationsItemPeekWhenMuted:
      title = prv_peek_when_muted_title();
      subtitle = quick_view_prefs_get_notif_muted_enabled() ? i18n_noop("On") : i18n_noop("Off");
      break;
#endif
    case NotificationsItemTextSize:
      /// String within Settings->Notifications that describes the text font size
      title = i18n_noop("Text Size");
      subtitle = s_text_size_names[prv_text_size_get_selection_index()];
      break;
    case NotificationsItemWindowTimeout: {
      title = prv_window_timeout_title();
      subtitle = s_window_timeouts_labels[prv_window_timeout_get_selection_index()];
      break;
    }
#if PBL_BW
    case NotificationsItemDesignStyle: {
      /// String within Settings->Notifications that describes the notification design style
      title = i18n_noop("Banner Style");
      subtitle = s_design_style_labels[prv_design_style_get_selection_index()];
      break;
    }
#endif /* PBL_BW */
    case NotificationsItemVibeDelay: {
      title = prv_vibe_delay_title();
      subtitle = s_vibe_delay_labels[prv_vibe_delay_get_selection_index()];
      break;
    }
    case NotificationsItemBacklight: {
      /// String within Settings->Notifications that describes backlight setting
      title = i18n_noop("Backlight");
      subtitle =
          alerts_preferences_get_notification_backlight() ? i18n_noop("On") : i18n_noop("Off");
      break;
    }
    case NotificationsItemGroupBySender: {
      /// Notification settings item for grouping notifications by sender
      title = i18n_noop("Group by Sender");
      subtitle = s_notification_grouping_range_labels
          [alerts_preferences_get_notification_grouping_range()];
      break;
    }
    case NotificationsItemStatusBarStyle: {
      /// String within Settings->Notifications that selects the notification status bar style
      title = i18n_noop("Status Bar");
      subtitle = s_status_bar_style_labels[prv_status_bar_style_get_selection_index()];
      break;
    }
    default:
      pbl_panic();
  }

  menu_cell_basic_draw(ctx, cell_layer, i18n_get(title, data), i18n_get(subtitle, data), nullptr);
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsNotificationsData *data = (SettingsNotificationsData *)context;
  i18n_free_all(data);
  app_free(data);
}

//! Reloads the rows and selects the setting that was selected, wherever it is now.
static void prv_reload_rows(SettingsNotificationsData *data) {
  settings_menu_reload_rows(SettingsMenuItemNotifications, prv_row_for_item(data->selected_item));
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsNotificationsData *data = (SettingsNotificationsData *)context;

  data->selected_item = prv_item_for_row(row);
  switch (data->selected_item) {
    case NotificationsItemFilter:
      prv_filter_menu_push(data);
      break;
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
    case NotificationsItemShowAs:
      prv_show_as_menu_push(data);
      break;
    case NotificationsItemPeekDuration:
      settings_quick_view_notif_seconds_menu_push(i18n_noop("Peek Duration"), data);
      break;
    case NotificationsItemPeekWhenMuted:
      settings_quick_view_notif_muted_toggle();
      break;
    case NotificationsItemPeekIfMissed:
      settings_quick_view_notif_missed_toggle();
      break;
#endif
    case NotificationsItemTextSize:
      prv_text_size_menu_push(data);
      break;
    case NotificationsItemWindowTimeout:
      prv_window_timeout_menu_push(data);
      break;
#if PBL_BW
    case NotificationsItemDesignStyle:
      prv_design_style_menu_push(data);
      break;
#endif /* PBL_BW */
    case NotificationsItemVibeDelay:
      prv_vibe_delay_menu_push(data);
      break;
    case NotificationsItemBacklight:
      // Toggle backlight directly without submenu
      alerts_preferences_set_notification_backlight(
          !alerts_preferences_get_notification_backlight());
      break;
    case NotificationsItemGroupBySender:
      prv_notification_grouping_range_menu_push(data);
      break;
    case NotificationsItemStatusBarStyle:
      prv_status_bar_style_menu_push(data);
      break;
    default:
      pbl_panic();
  }
  prv_reload_rows(data);
}

static void prv_settings_notifications_event_handler(PebbleEvent *event, void *context) {
  switch (event->type) {
    case PEBBLE_BATTERY_CONNECTION_EVENT:
      // Redraw the menu so that the Vibration status will be re-rendered.
      settings_menu_mark_dirty(SettingsMenuItemNotifications);
      break;
    default:
      break;
  }
}

static void prv_expand_cb(SettingsCallbacks *context) {
  SettingsNotificationsData *data = (SettingsNotificationsData *)context;

  data->battery_connection_event_info = (EventServiceInfo){
    .type = PEBBLE_BATTERY_CONNECTION_EVENT,
    .handler = prv_settings_notifications_event_handler,
  };
  event_service_client_subscribe(&data->battery_connection_event_info);
}

static void prv_appear_cb(SettingsCallbacks *context) {
  // Option menus and the Quiet Time page change which rows show, and where
  prv_reload_rows((SettingsNotificationsData *)context);
}

static void prv_hide_cb(SettingsCallbacks *context) {
  SettingsNotificationsData *data = (SettingsNotificationsData *)context;

  event_service_client_unsubscribe(&data->battery_connection_event_info);
}

static Window *prv_init(void) {
  SettingsNotificationsData *data = app_malloc_check(sizeof(*data));
  *data = (SettingsNotificationsData){};

  data->callbacks = (SettingsCallbacks){
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
    .expand = prv_expand_cb,
    .hide = prv_hide_cb,
    .appear = prv_appear_cb,
  };

  return settings_window_create(SettingsMenuItemNotifications, &data->callbacks);
}

const SettingsModuleMetadata *settings_notifications_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("Notifications"),
    .init = prv_init,
  };

  return &s_module_info;
}
