/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/peek_widgets.h"

#include "apps/system_app_ids.h"
#include "kernel/events.h"
#include "popups/timeline/peek.h"
#include "resource/timeline_resource_ids.auto.h"
#include "pbl/services/music.h"
#include "pbl/services/notifications/alerts.h"
#include "pbl/services/notifications/alerts_preferences.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
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
  Uuid installed_app_uuid;
  AppInstallId installed_app_id;
  // Notification storage
  bool notif_exists;
  Uuid stored_notif_id;
  // Watchface hook
  unsigned int num_button_state_calls;
} PeekWidgetsTestState;

static PeekWidgetsTestState s_test;

// Popup fakes (the real popup is not compiled into this test)
////////////////////////////////////////////////////////////////

static void prv_record_content(PeekWidgetSource source, TimelineItem *item) {
  s_test.last_source = source;
  s_test.num_content_calls++;
  s_test.last_title[0] = '\0';
  s_test.last_subtitle[0] = '\0';
  s_test.last_item_id = UUID_INVALID;
  if (item) {
    const char *title = attribute_get_string(&item->attr_list, AttributeIdTitle, "");
    const char *subtitle = attribute_get_string(&item->attr_list, AttributeIdSubtitle, "");
    strncpy(s_test.last_title, title, sizeof(s_test.last_title) - 1);
    strncpy(s_test.last_subtitle, subtitle, sizeof(s_test.last_subtitle) - 1);
    s_test.last_item_id = item->header.id;
  }
}

void timeline_peek_set_item(TimelineItem *item, bool started, unsigned int num_concurrent,
                            bool first, bool animated) {
  prv_record_content(item ? PeekWidgetSource_Timeline : PeekWidgetSource_None, item);
}

void timeline_peek_set_widget_item(PeekWidgetSource source, TimelineItem *item, bool animated) {
  cl_assert(item != NULL);
  cl_assert(source != PeekWidgetSource_Timeline);
  cl_assert(source != PeekWidgetSource_None);
  prv_record_content(source, item);
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
  return INSTALL_ID_INVALID;
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
};

bool notification_storage_get(const Uuid *id, TimelineItem *item_out) {
  if (!s_test.notif_exists || !uuid_equal(id, &s_test.stored_notif_id)) {
    return false;
  }
  *item_out = (TimelineItem) {
    .header = {
      .id = *id,
      .timestamp = rtc_get_time(),
      .type = TimelineItemTypeNotification,
      .layout = LayoutIdNotification,
    },
    .attr_list = {
      .num_attributes = ARRAY_LENGTH(s_notif_attrs),
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
    .icon = TIMELINE_RESOURCE_NOTIFICATION_FLAG,
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
