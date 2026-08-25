/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/peek_widgets.h"

#include "apps/system_app_ids.h"
#include "kernel/events.h"
#include "popups/timeline/peek.h"
#include "resource/resource_ids.auto.h"
#include "resource/timeline_resource_ids.auto.h"
#include "pbl/services/music.h"
#include "pbl/services/notifications/alerts.h"
#include "pbl/services/notifications/alerts_preferences.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
#include "pbl/services/timeline/notification_layout.h"
#include "pbl/services/timeline/timeline_resources.h"
#include "process_management/app_install_manager.h"
#include "shell/prefs.h"

#include "clar.h"
#include "pebble_asserts.h"

#include <string.h>

#include "pbl/util/size.h"

// Stubs
////////////////////////////////////////////////////////////////
// Note: the music/alerts/DND/app-install/pin-db/notification-storage functions the arbiter
// consumes are implemented below as controllable fakes instead of including their stub headers.
#include "stubs_event_loop.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_modal_manager.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_tasks.h"
#include "stubs_rand_ptr.h"

// Fakes
////////////////////////////////////////////////////////////////
#include "fake_new_timer.h"
#include "fake_pbl_malloc.h"
#include "fake_rtc.h"

// Fake state driving the WEAK stub overrides and popup/pref fakes below
////////////////////////////////////////////////////////////////

typedef struct PeekWidgetsTestState {
  // Popup fake
  PeekWidgetSource last_source;   //!< Source of the last content set (None = hidden)
  char last_title[64];
  char last_subtitle[64];
  Uuid last_item_id;
  Uuid last_parent_id;            //!< The item's app id, used to resolve published icons
  uint32_t last_icon;             //!< AttributeIdIconTiny on the item, 0 when absent
  bool last_has_icon_override;
  AppResourceInfo last_icon_override;
  unsigned int num_content_calls;
  bool timeline_enabled;
  bool last_future_empty;
  // Prefs
  QuickViewMusicMode music_mode;
  uint16_t notif_seconds;
  bool notif_muted;
  bool apps_enabled;
  QuickViewButtonMode button_mode;
  // Music service
  bool has_now_playing;
  MusicPlayState play_state;
  char music_title[64];
  char music_artist[64];
  uint8_t music_generation;
  // Environment
  bool watchface_running;
  bool alerts_allow;
  bool dnd_active;
  DndNotificationMode dnd_mode;
  bool pin_exists;
  bool notif_pin_exists;          //!< Whether the notification's pin resolves to an app
  uint32_t app_icon_resource;     //!< The installed app's own icon resource
  Uuid installed_app_uuid;
  AppInstallId installed_app_id;
  // Notification storage
  bool notif_exists;
  uint32_t notif_icon;            //!< The notification's own icon; 0 = it carries none
  Uuid stored_notif_id;
  // Watchface hook
  unsigned int num_button_state_calls;
} PeekWidgetsTestState;

// The Music app's identity and icon, as the real app registry would report them
static const Uuid s_music_app_uuid = {
  0x1f, 0x03, 0x29, 0x3d, 0x47, 0xaf, 0x4f, 0x28,
  0xb9, 0x60, 0xf2, 0xb0, 0x2a, 0x6d, 0xd7, 0x57,
};
#define MUSIC_APP_ICON_RESOURCE (4242)
#define SYSTEM_APP_ICON_BANK (0)
#define APP_ICON_BANK (7)
#define APP_OWN_ICON_RESOURCE (99)

static PeekWidgetsTestState s_test;

// Popup fakes (the real popup is not compiled into this test)
////////////////////////////////////////////////////////////////

static void prv_record_content(PeekWidgetSource source, TimelineItem *item,
                               const AppResourceInfo *icon_override) {
  s_test.last_source = source;
  s_test.num_content_calls++;
  s_test.last_title[0] = '\0';
  s_test.last_subtitle[0] = '\0';
  s_test.last_item_id = UUID_INVALID;
  s_test.last_parent_id = UUID_INVALID;
  s_test.last_icon = 0;
  s_test.last_has_icon_override = (icon_override != NULL);
  s_test.last_icon_override = icon_override ? *icon_override : (AppResourceInfo) {};
  if (item) {
    const char *title = attribute_get_string(&item->attr_list, AttributeIdTitle, "");
    const char *subtitle = attribute_get_string(&item->attr_list, AttributeIdSubtitle, "");
    strncpy(s_test.last_title, title, sizeof(s_test.last_title) - 1);
    strncpy(s_test.last_subtitle, subtitle, sizeof(s_test.last_subtitle) - 1);
    s_test.last_item_id = item->header.id;
    s_test.last_parent_id = item->header.parent_id;
    s_test.last_icon = attribute_get_uint32(&item->attr_list, AttributeIdIconTiny, 0);
  }
}

void timeline_peek_set_item(TimelineItem *item, bool started, unsigned int num_concurrent,
                            bool first, bool animated) {
  prv_record_content(item ? PeekWidgetSource_Timeline : PeekWidgetSource_None, item,
                     NULL /* icon_override */);
}

void timeline_peek_set_widget_item(PeekWidgetSource source, TimelineItem *item,
                                   const AppResourceInfo *icon_override, bool animated) {
  cl_assert(item != NULL);
  cl_assert(source != PeekWidgetSource_Timeline);
  cl_assert(source != PeekWidgetSource_None);
  prv_record_content(source, item, icon_override);
}

bool timeline_peek_is_enabled(void) {
  return s_test.timeline_enabled;
}

void timeline_peek_note_event_flags(bool is_future_empty) {
  s_test.last_future_empty = is_future_empty;
}

// Watchface fake
////////////////////////////////////////////////////////////////

void watchface_peek_widget_state_changed(void) {
  s_test.num_button_state_calls++;
}

// Layout registry fake (pulled in by item.c)
////////////////////////////////////////////////////////////////

bool layout_verify(bool existing_attributes[], LayoutId id) {
  return true;
}

// Pref fakes
////////////////////////////////////////////////////////////////

QuickViewMusicMode quick_view_prefs_get_music_mode(void) {
  return s_test.music_mode;
}

uint16_t quick_view_prefs_get_notif_seconds(void) {
  return s_test.notif_seconds;
}

bool quick_view_prefs_get_notif_muted_enabled(void) {
  return s_test.notif_muted;
}

bool quick_view_prefs_get_apps_enabled(void) {
  return s_test.apps_enabled;
}

QuickViewButtonMode quick_view_prefs_get_button_mode(void) {
  return s_test.button_mode;
}

// WEAK stub overrides
////////////////////////////////////////////////////////////////

bool music_has_now_playing(void) {
  return s_test.has_now_playing;
}

MusicPlayState music_get_playback_state(void) {
  return s_test.play_state;
}

void music_get_now_playing(char *title, char *artist, char *album) {
  strcpy(title, s_test.music_title);
  strcpy(artist, s_test.music_artist);
  album[0] = '\0';
}

uint8_t music_get_now_playing_generation(void) {
  return s_test.music_generation;
}

bool app_manager_is_watchface_running(void) {
  return s_test.watchface_running;
}

bool alerts_should_notify_for_type(AlertType type) {
  return s_test.alerts_allow;
}

bool do_not_disturb_is_active(void) {
  return s_test.dnd_active;
}

DndNotificationMode alerts_preferences_dnd_get_show_notifications(void) {
  return s_test.dnd_mode;
}

AppInstallId app_install_get_id_for_uuid(const Uuid *uuid) {
  if (uuid_equal(uuid, &s_test.installed_app_uuid)) {
    return s_test.installed_app_id;
  }
  if (uuid_equal(uuid, &s_music_app_uuid)) {
    return APP_ID_MUSIC;
  }
  return INSTALL_ID_INVALID;
}

bool app_install_get_entry_for_install_id(AppInstallId install_id, AppInstallEntry *entry) {
  if (install_id == s_test.installed_app_id) {
    *entry = (AppInstallEntry) { .uuid = s_test.installed_app_uuid };
    return true;
  }
  if (install_id == APP_ID_MUSIC) {
    *entry = (AppInstallEntry) { .uuid = s_music_app_uuid };
    return true;
  }
  return false;
}

uint32_t app_install_entry_get_icon_resource_id(const AppInstallEntry *entry) {
  if (uuid_equal(&entry->uuid, &s_music_app_uuid)) {
    return MUSIC_APP_ICON_RESOURCE;
  }
  return s_test.app_icon_resource;
}

ResAppNum app_install_get_app_icon_bank(const AppInstallEntry *entry) {
  return uuid_equal(&entry->uuid, &s_music_app_uuid) ? SYSTEM_APP_ICON_BANK
                                                     : APP_ICON_BANK;
}

status_t pin_db_read_item_header(TimelineItem *item_out, TimelineItemId *id) {
  if (!s_test.notif_pin_exists) {
    return E_DOES_NOT_EXIST;
  }
  *item_out = (TimelineItem) {
    .header = {
      .id = *id,
      .parent_id = s_test.installed_app_uuid,
    },
  };
  return S_SUCCESS;
}

bool timeline_resources_is_system(TimelineResourceId timeline_id) {
  return (timeline_id & SYSTEM_RESOURCE_FLAG) != 0;
}

TimelineResourceId notification_layout_get_fallback_icon_id(TimelineItemType type) {
  return NOTIF_FALLBACK_ICON;
}

static Attribute s_pin_title_attr = {
  .id = AttributeIdTitle,
  .cstring = "Standup",
};

status_t pin_db_get(const TimelineItemId *id, TimelineItem *pin_out) {
  if (!s_test.pin_exists) {
    return E_DOES_NOT_EXIST;
  }
  *pin_out = (TimelineItem) {
    .header = {
      .id = *id,
      .timestamp = rtc_get_time(),
      .duration = 30,
      .type = TimelineItemTypePin,
      .layout = LayoutIdGeneric,
    },
    .attr_list = {
      .num_attributes = 1,
      .attributes = &s_pin_title_attr,
    },
  };
  return S_SUCCESS;
}

static Attribute s_notif_attrs[] = {
  { .id = AttributeIdTitle, .cstring = "Alice" },
  { .id = AttributeIdSubtitle, .cstring = "Lunch?" },
  { .id = AttributeIdIconTiny, .uint32 = 0 },
};

bool notification_storage_get(const Uuid *id, TimelineItem *item_out) {
  if (!s_test.notif_exists || !uuid_equal(id, &s_test.stored_notif_id)) {
    return false;
  }
  s_notif_attrs[2].uint32 = s_test.notif_icon;
  *item_out = (TimelineItem) {
    .header = {
      .id = *id,
      .timestamp = rtc_get_time(),
      .type = TimelineItemTypeNotification,
      .layout = LayoutIdNotification,
    },
    .attr_list = {
      // The icon attribute is only present when the notification actually carries one
      .num_attributes = s_test.notif_icon ? ARRAY_LENGTH(s_notif_attrs)
                                          : (ARRAY_LENGTH(s_notif_attrs) - 1),
      .attributes = s_notif_attrs,
    },
  };
  return true;
}

// Helpers
////////////////////////////////////////////////////////////////

static const Uuid s_notif_uuid = {
  0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

static const Uuid s_pin_uuid = {
  0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
};

static const Uuid s_app_uuid = {
  0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
};

static void prv_send_media_event(PebbleMediaEventType type) {
  PebbleMediaEvent event = { .type = type };
  peek_widgets_handle_media_event(&event);
}

static void prv_change_track(const char *title, const char *artist) {
  strcpy(s_test.music_title, title);
  strcpy(s_test.music_artist, artist);
  s_test.music_generation++;
  prv_send_media_event(PebbleMediaEventTypeNowPlayingChanged);
}

static void prv_send_notification_added(const Uuid *id) {
  Uuid id_copy = *id;
  PebbleSysNotificationEvent event = {
    .type = NotificationAdded,
    .notification_id = &id_copy,
  };
  peek_widgets_handle_notification_event(&event);
}

static void prv_send_notification_removed(const Uuid *id) {
  Uuid id_copy = *id;
  PebbleSysNotificationEvent event = {
    .type = NotificationRemoved,
    .notification_id = &id_copy,
  };
  peek_widgets_handle_notification_event(&event);
}

static void prv_send_timeline_event(const Uuid *item_id, TimelinePeekTimeType time_type) {
  Uuid id_copy;
  PebbleTimelinePeekEvent event = {
    .time_type = time_type,
    .num_concurrent = 0,
    .is_first_event = true,
    .is_future_empty = false,
  };
  if (item_id) {
    id_copy = *item_id;
    event.item_id = &id_copy;
  }
  peek_widgets_handle_timeline_peek_event(&event);
}

static void prv_publish_app_widget(const Uuid *owner, const char *title, uint32_t launch_code,
                                   uint16_t timeout_s) {
  const PeekWidgetAppPublish publish = {
    .owner = *owner,
    .icon = PEEK_WIDGET_APP_ICON_DEFAULT,
    .title = title,
    .launch_code = launch_code,
    .timeout_s = timeout_s,
  };
  cl_assert(peek_widgets_publish_app_widget(&publish));
}

static void prv_start_playing(void) {
  s_test.has_now_playing = true;
  s_test.play_state = MusicPlayStatePlaying;
  prv_change_track("Track A", "Artist A");
}

// Setup
////////////////////////////////////////////////////////////////

void test_peek_widgets__initialize(void) {
  fake_rtc_init(0 /* initial_ticks */, 1000000 /* initial_time */);
  s_test = (PeekWidgetsTestState) {
    .last_source = PeekWidgetSource_None,
    .timeline_enabled = true,
    .music_mode = QuickViewMusicMode_WhilePlaying,
    .notif_seconds = QUICK_VIEW_NOTIF_SECONDS_DEFAULT,
    .apps_enabled = true,
    .button_mode = QuickViewButtonMode_Press,
    .play_state = MusicPlayStateUnknown,
    .watchface_running = true,
    .alerts_allow = true,
    .dnd_mode = DndNotificationModeShow,
    .stored_notif_id = s_notif_uuid,
    .installed_app_uuid = s_app_uuid,
    .installed_app_id = 1234,
    .app_icon_resource = APP_OWN_ICON_RESOURCE,
  };
  peek_widgets_init();
}

void test_peek_widgets__cleanup(void) {
}

// Tests: music
////////////////////////////////////////////////////////////////

void test_peek_widgets__music_shows_while_playing(void) {
  prv_start_playing();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);
  cl_assert_equal_s(s_test.last_title, "Track A");
  cl_assert_equal_s(s_test.last_subtitle, "Artist A");

  s_test.play_state = MusicPlayStatePaused;
  prv_send_media_event(PebbleMediaEventTypePlaybackStateChanged);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__music_hides_on_disconnect(void) {
  prv_start_playing();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);

  s_test.has_now_playing = false;
  s_test.play_state = MusicPlayStateUnknown;
  prv_send_media_event(PebbleMediaEventTypeServerDisconnected);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__music_disabled_pref(void) {
  s_test.music_mode = QuickViewMusicMode_Disabled;
  prv_start_playing();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__music_track_start_mode(void) {
  s_test.music_mode = QuickViewMusicMode_TrackStart;
  prv_start_playing();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);

  // The wake timer is armed for the end of the 10 s window; firing it hides the widget.
  fake_rtc_increment_time(11);
  stub_new_timer_invoke(1 /* num_to_invoke */);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);

  // The next track brings it back.
  prv_change_track("Track B", "Artist B");
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);
  cl_assert_equal_s(s_test.last_title, "Track B");
}

void test_peek_widgets__music_track_change_updates_content(void) {
  prv_start_playing();
  cl_assert_equal_s(s_test.last_title, "Track A");
  prv_change_track("Track B", "Artist B");
  cl_assert_equal_s(s_test.last_title, "Track B");
}

void test_peek_widgets__music_dismiss_latches_until_track_change(void) {
  prv_start_playing();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);

  cl_assert(peek_widgets_handle_dismiss());
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);

  // Still dismissed for the same track.
  prv_send_media_event(PebbleMediaEventTypePlaybackStateChanged);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);

  // A new track clears the latch.
  prv_change_track("Track B", "Artist B");
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);
}

// Tests: timeline
////////////////////////////////////////////////////////////////

void test_peek_widgets__timeline_shows_and_hides(void) {
  s_test.pin_exists = true;
  prv_send_timeline_event(&s_pin_uuid, TimelinePeekTimeType_ShowWillStart);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Timeline);

  prv_send_timeline_event(NULL, TimelinePeekTimeType_None);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__timeline_outranks_music(void) {
  prv_start_playing();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);

  s_test.pin_exists = true;
  prv_send_timeline_event(&s_pin_uuid, TimelinePeekTimeType_ShowStarted);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Timeline);

  // When the event ends, music returns.
  prv_send_timeline_event(NULL, TimelinePeekTimeType_None);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);
}

void test_peek_widgets__timeline_disabled_lets_music_win(void) {
  s_test.timeline_enabled = false;
  prv_start_playing();
  s_test.pin_exists = true;
  prv_send_timeline_event(&s_pin_uuid, TimelinePeekTimeType_ShowWillStart);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);
}

void test_peek_widgets__timeline_missing_pin_falls_through(void) {
  prv_start_playing();
  s_test.pin_exists = false;
  prv_send_timeline_event(&s_pin_uuid, TimelinePeekTimeType_ShowWillStart);
  // The pin vanished before it could be displayed; music takes the surface.
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);
}

// Tests: notifications
////////////////////////////////////////////////////////////////

void test_peek_widgets__notification_outranks_all_and_expires(void) {
  prv_start_playing();
  s_test.pin_exists = true;
  prv_send_timeline_event(&s_pin_uuid, TimelinePeekTimeType_ShowStarted);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Timeline);

  s_test.notif_exists = true;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Notification);
  cl_assert_equal_s(s_test.last_title, "Alice");
  cl_assert_equal_s(s_test.last_subtitle, "Lunch?");

  // After the 15 s default window, the timeline content returns.
  fake_rtc_increment_time(16);
  stub_new_timer_invoke(1 /* num_to_invoke */);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Timeline);
}

void test_peek_widgets__notification_armed_until_watchface_focus(void) {
  // The notification modal has focus when the notification arrives.
  s_test.watchface_running = false;
  s_test.notif_exists = true;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);

  // Focus returns to the watchface: the display window starts now.
  s_test.watchface_running = true;
  PebbleAppFocusEvent focus_event = { .in_focus = true };
  peek_widgets_handle_app_focus_event(&focus_event);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Notification);
}

void test_peek_widgets__notification_removed_clears_widget(void) {
  s_test.notif_exists = true;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Notification);

  prv_send_notification_removed(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__notification_respects_gates(void) {
  s_test.notif_exists = true;

  // Disabled by pref
  s_test.notif_seconds = 0;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
  s_test.notif_seconds = QUICK_VIEW_NOTIF_SECONDS_DEFAULT;

  // Muted by the alerts mask
  s_test.alerts_allow = false;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
  s_test.alerts_allow = true;

  // Hidden during Quiet Time
  s_test.dnd_active = true;
  s_test.dnd_mode = DndNotificationModeHide;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__notification_show_when_muted(void) {
  s_test.notif_muted = true;
  s_test.notif_exists = true;

  // Muted by the alerts mask: no popup would show, so the widget appears immediately
  s_test.alerts_allow = false;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Notification);
  cl_assert_equal_s(s_test.last_title, "Alice");
  cl_assert(peek_widgets_handle_dismiss());

  // Hidden during Quiet Time: same silent residue
  s_test.alerts_allow = true;
  s_test.dnd_active = true;
  s_test.dnd_mode = DndNotificationModeHide;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Notification);
  cl_assert(peek_widgets_handle_dismiss());

  // The duration pref still disables the widget entirely
  s_test.notif_seconds = 0;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__notification_persistent_until_dismissed(void) {
  s_test.notif_seconds = QUICK_VIEW_NOTIF_SECONDS_PERSISTENT;
  s_test.notif_exists = true;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Notification);

  // Hours later it is still up.
  fake_rtc_increment_time(60 * 60);
  peek_widgets_handle_prefs_changed();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Notification);

  cl_assert(peek_widgets_handle_dismiss());
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

// Tests: app widgets
////////////////////////////////////////////////////////////////

void test_peek_widgets__app_widget_publish_and_withdraw(void) {
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 42, 0 /* no timeout */);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);
  cl_assert_equal_s(s_test.last_title, "Kettle ready");

  peek_widgets_withdraw_app_widget(&s_app_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__app_widget_expires(void) {
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 0, 60 /* timeout_s */);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);

  fake_rtc_increment_time(61);
  peek_widgets_handle_prefs_changed();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__app_widget_below_timeline_above_music(void) {
  prv_start_playing();
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 0, 0);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);

  s_test.pin_exists = true;
  prv_send_timeline_event(&s_pin_uuid, TimelinePeekTimeType_ShowStarted);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Timeline);

  prv_send_timeline_event(NULL, TimelinePeekTimeType_None);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);
}

void test_peek_widgets__app_widget_master_toggle(void) {
  s_test.apps_enabled = false;
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 0, 0);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__app_widget_uninstalled_app_dropped(void) {
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 0, 0);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);

  s_test.installed_app_uuid = UUID_INVALID;
  peek_widgets_handle_prefs_changed();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);
}

void test_peek_widgets__app_widget_dismiss(void) {
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 0, 0);
  cl_assert(peek_widgets_handle_dismiss());
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_None);

  // Republishing revives the widget.
  prv_publish_app_widget(&s_app_uuid, "Tea steeped", 0, 0);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);
  cl_assert_equal_s(s_test.last_title, "Tea steeped");
}

// Tests: icons
////////////////////////////////////////////////////////////////

void test_peek_widgets__notification_wears_its_own_icon(void) {
  // A system icon (the sender's, e.g. the SMS glyph) is carried through untouched
  s_test.notif_exists = true;
  s_test.notif_icon = SYSTEM_RESOURCE_FLAG | 45;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Notification);
  cl_assert_equal_i(s_test.last_icon, SYSTEM_RESOURCE_FLAG | 45);
  cl_assert(!s_test.last_has_icon_override);
}

void test_peek_widgets__notification_without_icon_uses_notification_fallback(void) {
  s_test.notif_exists = true;
  s_test.notif_icon = 0;
  prv_send_notification_added(&s_notif_uuid);
  // The notification list's fallback, not the generic layout's timeline pin flag
  cl_assert_equal_i(s_test.last_icon, NOTIF_FALLBACK_ICON);
}

void test_peek_widgets__notification_app_icon_resolves_through_the_pin(void) {
  // An app-published icon id only means something against the publishing app's UUID, which
  // lives on the pin the notification belongs to, not on the notification itself.
  s_test.notif_exists = true;
  s_test.notif_icon = 7;
  s_test.notif_pin_exists = true;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_icon, 7);
  cl_assert(uuid_equal(&s_test.last_parent_id, &s_app_uuid));

  // Without the pin the id cannot be resolved, so fall back rather than show a wrong icon
  cl_assert(peek_widgets_handle_dismiss());
  s_test.notif_pin_exists = false;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert_equal_i(s_test.last_icon, NOTIF_FALLBACK_ICON);
}

void test_peek_widgets__music_wears_the_music_app_icon(void) {
  prv_start_playing();
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Music);
  cl_assert(s_test.last_has_icon_override);
  cl_assert_equal_i(s_test.last_icon_override.res_id, MUSIC_APP_ICON_RESOURCE);
  cl_assert_equal_i(s_test.last_icon_override.res_app_num, SYSTEM_APP_ICON_BANK);
}

void test_peek_widgets__app_widget_uses_its_published_icon(void) {
  const PeekWidgetAppPublish publish = {
    .owner = s_app_uuid,
    .icon = 3,
    .title = "Kettle ready",
  };
  cl_assert(peek_widgets_publish_app_widget(&publish));
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);
  // Resolved as published media against the publishing app
  cl_assert_equal_i(s_test.last_icon, 3);
  cl_assert(uuid_equal(&s_test.last_parent_id, &s_app_uuid));
  cl_assert(!s_test.last_has_icon_override);
}

void test_peek_widgets__app_widget_defaults_to_the_app_icon(void) {
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 0 /* launch_code */, 0 /* timeout_s */);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);
  cl_assert_equal_i(s_test.last_icon, 0);
  cl_assert(s_test.last_has_icon_override);
  cl_assert_equal_i(s_test.last_icon_override.res_id, APP_OWN_ICON_RESOURCE);
  cl_assert_equal_i(s_test.last_icon_override.res_app_num, APP_ICON_BANK);
}

void test_peek_widgets__app_widget_without_an_app_icon_falls_back(void) {
  s_test.app_icon_resource = 0;
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 0 /* launch_code */, 0 /* timeout_s */);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_App);
  // The app has no icon of its own, so use the launcher's generic app icon rather than
  // letting the layout fall back to a timeline pin icon.
  cl_assert(s_test.last_has_icon_override);
  cl_assert_equal_i(s_test.last_icon_override.res_id,
                    RESOURCE_ID_MENU_LAYER_GENERIC_WATCHAPP_ICON);
}

// Tests: launch info
////////////////////////////////////////////////////////////////

void test_peek_widgets__get_launch_per_source(void) {
  PeekWidgetLaunch launch;

  // Nothing showing
  cl_assert(!peek_widgets_get_launch(&launch));

  // Music
  prv_start_playing();
  cl_assert(peek_widgets_get_launch(&launch));
  cl_assert_equal_i(launch.app_id, APP_ID_MUSIC);
  cl_assert_equal_i(launch.launch_code, 0);

  // App widget with a launch code
  prv_publish_app_widget(&s_app_uuid, "Kettle ready", 42, 0);
  cl_assert(peek_widgets_get_launch(&launch));
  cl_assert_equal_i(launch.app_id, 1234);
  cl_assert_equal_i(launch.launch_code, 42);

  // Timeline never provides a widget launch; its deep-link stays in the shell.
  s_test.pin_exists = true;
  prv_send_timeline_event(&s_pin_uuid, TimelinePeekTimeType_ShowStarted);
  cl_assert(!peek_widgets_get_launch(&launch));

  // Notification launches the notifications app
  s_test.notif_exists = true;
  prv_send_notification_added(&s_notif_uuid);
  cl_assert(peek_widgets_get_launch(&launch));
  cl_assert_equal_i(launch.app_id, APP_ID_NOTIFICATIONS);
}

void test_peek_widgets__dismiss_does_nothing_for_timeline(void) {
  s_test.pin_exists = true;
  prv_send_timeline_event(&s_pin_uuid, TimelinePeekTimeType_ShowStarted);
  cl_assert_equal_i(s_test.last_source, PeekWidgetSource_Timeline);
  cl_assert(!peek_widgets_handle_dismiss());
}

void test_peek_widgets__button_state_hook_fires(void) {
  const unsigned int before = s_test.num_button_state_calls;
  prv_start_playing();
  cl_assert(s_test.num_button_state_calls > before);
}
