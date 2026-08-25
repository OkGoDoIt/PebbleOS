/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/peek_widgets.h"

#include <pbl/drivers/rtc.h>
#include "apps/system_app_ids.h"
#include "kernel/event_loop.h"
#include "kernel/ui/modals/modal_manager.h"
#include "popups/timeline/peek.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_manager.h"
#include "resource/timeline_resource_ids.auto.h"
#include "pbl/os/mutex.h"
#include "pbl/services/music.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/notifications/alerts.h"
#include "pbl/services/notifications/alerts_preferences.h"
#include "pbl/services/notifications/do_not_disturb.h"
#include "pbl/services/notifications/notification_storage.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
#include "pbl/services/blob_db/pin_db.h"
#include "shell/normal/watchface.h"
#include "shell/prefs.h"
#include <pbl/logging/logging.h>
#include "system/passert.h"
#include "pbl/util/math.h"
#include "util/time/time.h"

#include <string.h>

PBL_LOG_MODULE_DEFINE(service_peek_widgets, CONFIG_SERVICE_PEEK_WIDGETS_LOG_LEVEL);

//! How long the music widget stays up after a track change in Start of Track mode.
#define PEEK_WIDGET_TRACK_START_SHOW_S (10)

#define PEEK_WIDGET_MAX_APP_WIDGETS (8)

//! Item id for the music widget's synthetic content. App widgets use their owner UUID and the
//! notification widget uses the notification UUID, so content changes animate naturally.
static const Uuid s_music_widget_item_id = {
  0x8f, 0x1c, 0x5e, 0x0a, 0x3d, 0x27, 0x4b, 0x9e,
  0x91, 0x64, 0x2b, 0x7a, 0xc4, 0x55, 0x08, 0x31,
};

typedef struct AppPeekWidget {
  bool in_use;
  bool dismissed;
  Uuid owner;
  uint32_t icon;
  char title[PEEK_WIDGET_APP_TITLE_MAX_LEN + 1];
  char subtitle[PEEK_WIDGET_APP_SUBTITLE_MAX_LEN + 1];
  uint32_t launch_code;
  time_t published_at;
  time_t expires_at; //!< 0 = no expiry
} AppPeekWidget;

typedef struct PeekWidgetsState {
  bool initialized;
  //! What the overlay is currently showing.
  PeekWidgetSource shown;
  //! Set by a source handler when its content changed and the overlay needs a new layout.
  bool content_dirty;

  // Timeline candidate, replayed from the most recent timeline peek event.
  bool timeline_active;
  TimelineItemId timeline_item_id;
  bool timeline_started;
  uint8_t timeline_num_concurrent;
  bool timeline_first;

  // Music.
  bool music_dismissed;
  uint8_t music_dismissed_generation;
  bool music_generation_seen;
  uint8_t music_generation;
  time_t track_changed_at;

  // Notification.
  Uuid notif_id;
  bool notif_armed;  //!< Waiting for the watchface to regain focus.
  bool notif_active;
  time_t notif_expires_at; //!< 0 = no expiry (Until Dismissed)

  //! App-published widgets. Guarded by lock: publish/withdraw may run on any task.
  PebbleMutex *app_lock;
  AppPeekWidget app_widgets[PEEK_WIDGET_MAX_APP_WIDGETS];

  TimerID timer;
} PeekWidgetsState;

static PeekWidgetsState s_state;

static void prv_refresh(bool animated);

// Refresh marshaling
////////////////////////////////////////////////////////////////

static void prv_refresh_main_cb(void *data) {
  prv_refresh(true /* animated */);
}

static void prv_request_refresh(void) {
  launcher_task_add_callback(prv_refresh_main_cb, NULL);
}

static void prv_timer_cb(void *data) {
  prv_request_refresh();
}

// Music source
////////////////////////////////////////////////////////////////

static bool prv_music_is_active(void) {
  const QuickViewMusicMode mode = quick_view_prefs_get_music_mode();
  if (mode == QuickViewMusicMode_Disabled) {
    return false;
  }
  if (s_state.music_dismissed) {
    return false;
  }
  if (!music_has_now_playing()) {
    return false;
  }
  const MusicPlayState play_state = music_get_playback_state();
  const bool playing = (play_state == MusicPlayStatePlaying) ||
                       (play_state == MusicPlayStateForwarding) ||
                       (play_state == MusicPlayStateRewinding);
  if (!playing) {
    return false;
  }
  if (mode == QuickViewMusicMode_TrackStart) {
    return (rtc_get_time() < (s_state.track_changed_at + PEEK_WIDGET_TRACK_START_SHOW_S));
  }
  return true;
}

static void prv_show_music(bool animated) {
  char title[MUSIC_BUFFER_LENGTH];
  char artist[MUSIC_BUFFER_LENGTH];
  char album[MUSIC_BUFFER_LENGTH];
  music_get_now_playing(title, artist, album);

  AttributeList attr_list = {};
  attribute_list_add_cstring(&attr_list, AttributeIdTitle, title);
  if (artist[0] != '\0') {
    attribute_list_add_cstring(&attr_list, AttributeIdSubtitle, artist);
  }
  attribute_list_add_resource_id(&attr_list, AttributeIdIconTiny, TIMELINE_RESOURCE_MUSIC_EVENT);

  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0 /* duration */, TimelineItemTypePin, LayoutIdGeneric, &attr_list,
      NULL /* action_group */);
  attribute_list_destroy_list(&attr_list);
  if (!item) {
    return;
  }
  item->header.id = s_music_widget_item_id;
  timeline_peek_set_widget_item(PeekWidgetSource_Music, item, animated);
  timeline_item_destroy(item);
}

void peek_widgets_handle_media_event(PebbleMediaEvent *event) {
  switch (event->type) {
    case PebbleMediaEventTypeNowPlayingChanged: {
      const uint8_t generation = music_get_now_playing_generation();
      if (!s_state.music_generation_seen || (generation != s_state.music_generation)) {
        s_state.music_generation_seen = true;
        s_state.music_generation = generation;
        s_state.track_changed_at = rtc_get_time();
        if (s_state.music_dismissed && (generation != s_state.music_dismissed_generation)) {
          s_state.music_dismissed = false;
        }
        s_state.content_dirty = true;
      }
      break;
    }
    case PebbleMediaEventTypePlaybackStateChanged:
    case PebbleMediaEventTypeServerConnected:
    case PebbleMediaEventTypeServerDisconnected:
      break;
    default:
      // Volume/track position/album art changes don't affect the widget's layout.
      return;
  }
  prv_refresh(true /* animated */);
}

// Notification source
////////////////////////////////////////////////////////////////

static bool prv_watchface_has_focus(void) {
  return app_manager_is_watchface_running() &&
         (modal_manager_get_properties() & ModalProperty_Unfocused);
}

static void prv_notif_clear(void) {
  s_state.notif_armed = false;
  s_state.notif_active = false;
  s_state.notif_expires_at = 0;
}

static void prv_notif_activate(void) {
  const uint16_t seconds = quick_view_prefs_get_notif_seconds();
  if (seconds == 0) {
    prv_notif_clear();
    return;
  }
  s_state.notif_armed = false;
  s_state.notif_active = true;
  s_state.notif_expires_at =
      (seconds == QUICK_VIEW_NOTIF_SECONDS_PERSISTENT) ? 0 : (rtc_get_time() + seconds);
}

//! Builds the notification widget's content from the stored notification.
//! @return false when the notification no longer exists.
static bool prv_show_notification(bool animated) {
  TimelineItem notif = {};
  if (!notification_storage_get(&s_state.notif_id, &notif)) {
    return false;
  }

  const char *title = attribute_get_string(&notif.attr_list, AttributeIdTitle, NULL);
  const char *subtitle = attribute_get_string(&notif.attr_list, AttributeIdSubtitle, NULL);
  const char *body = attribute_get_string(&notif.attr_list, AttributeIdBody, NULL);
  const char *primary = title ?: subtitle ?: body;
  const char *secondary = title ? (subtitle ?: body) : (subtitle ? body : NULL);
  const uint32_t icon = attribute_get_uint32(&notif.attr_list, AttributeIdIconTiny,
                                             TIMELINE_RESOURCE_NOTIFICATION_GENERIC);
  if (!primary) {
    timeline_item_free_allocated_buffer(&notif);
    return false;
  }

  AttributeList attr_list = {};
  attribute_list_add_cstring(&attr_list, AttributeIdTitle, primary);
  if (secondary) {
    attribute_list_add_cstring(&attr_list, AttributeIdSubtitle, secondary);
  }
  attribute_list_add_resource_id(&attr_list, AttributeIdIconTiny, icon);

  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0 /* duration */, TimelineItemTypePin, LayoutIdGeneric, &attr_list,
      NULL /* action_group */);
  attribute_list_destroy_list(&attr_list);
  const bool shown = (item != NULL);
  if (item) {
    item->header.id = s_state.notif_id;
    item->header.parent_id = notif.header.parent_id;
    timeline_peek_set_widget_item(PeekWidgetSource_Notification, item, animated);
    timeline_item_destroy(item);
  }
  timeline_item_free_allocated_buffer(&notif);
  return shown;
}

void peek_widgets_handle_notification_event(PebbleSysNotificationEvent *event) {
  switch (event->type) {
    case NotificationAdded: {
      if (quick_view_prefs_get_notif_seconds() == 0) {
        return;
      }
      if (do_not_disturb_is_active() &&
          (alerts_preferences_dnd_get_show_notifications() == DndNotificationModeHide)) {
        return;
      }
      if (!alerts_should_notify_for_type(AlertMobile)) {
        return;
      }
      s_state.notif_id = *event->notification_id;
      s_state.notif_active = false;
      s_state.notif_armed = true;
      s_state.content_dirty = true;
      if (prv_watchface_has_focus()) {
        prv_notif_activate();
      }
      break;
    }
    case NotificationActedUpon:
    case NotificationRemoved:
      if (!uuid_equal(event->notification_id, &s_state.notif_id)) {
        return;
      }
      prv_notif_clear();
      break;
    default:
      return;
  }
  prv_refresh(true /* animated */);
}

static void prv_maybe_activate_armed_notification(void) {
  if (s_state.notif_armed && !s_state.notif_active && prv_watchface_has_focus()) {
    prv_notif_activate();
    prv_refresh(true /* animated */);
  }
}

void peek_widgets_handle_app_focus_event(PebbleAppFocusEvent *event) {
  if (event->in_focus) {
    prv_maybe_activate_armed_notification();
  }
}

static void prv_watchface_started_main_cb(void *data) {
  prv_maybe_activate_armed_notification();
}

void peek_widgets_handle_watchface_started(void) {
  // Called synchronously during the app switch; defer to a KernelMain callback.
  launcher_task_add_callback(prv_watchface_started_main_cb, NULL);
}

// Timeline source
////////////////////////////////////////////////////////////////

void peek_widgets_handle_timeline_peek_event(PebbleTimelinePeekEvent *event) {
  timeline_peek_note_event_flags(event->is_future_empty);
  bool active = false;
  bool started = false;
  if (event->item_id != NULL) {
    switch (event->time_type) {
      case TimelinePeekTimeType_None:
      case TimelinePeekTimeType_SomeTimeNext:
      case TimelinePeekTimeType_WillEnd:
        break;
      case TimelinePeekTimeType_ShowWillStart:
        active = true;
        break;
      case TimelinePeekTimeType_ShowStarted:
        active = true;
        started = true;
        break;
    }
  }
  s_state.timeline_active = active;
  if (active) {
    s_state.timeline_item_id = *event->item_id;
    s_state.timeline_started = started;
    s_state.timeline_num_concurrent = event->num_concurrent;
    s_state.timeline_first = event->is_first_event;
  }
  s_state.content_dirty = true;
  prv_refresh(true /* animated */);
}

//! @return false when the pin no longer exists.
static bool prv_show_timeline(bool animated) {
  TimelineItem item = {};
  const status_t rv = pin_db_get(&s_state.timeline_item_id, &item);
  if (rv != S_SUCCESS) {
    // The pin may have just been deleted; a follow-up peek event will normally correct us.
    return false;
  }
  timeline_peek_set_item(&item, s_state.timeline_started, s_state.timeline_num_concurrent,
                         s_state.timeline_first, animated);
  timeline_item_free_allocated_buffer(&item);
  return true;
}

// App source
////////////////////////////////////////////////////////////////

//! Picks the freshest live app widget. Expired and dismissed entries are dropped in place.
//! Must be called with app_lock held.
static AppPeekWidget *prv_pick_app_widget(void) {
  const time_t now = rtc_get_time();
  AppPeekWidget *best = NULL;
  for (unsigned int i = 0; i < PEEK_WIDGET_MAX_APP_WIDGETS; i++) {
    AppPeekWidget *widget = &s_state.app_widgets[i];
    if (!widget->in_use) {
      continue;
    }
    if (widget->expires_at && (now >= widget->expires_at)) {
      widget->in_use = false;
      continue;
    }
    if (widget->dismissed) {
      continue;
    }
    if (!best || (widget->published_at > best->published_at)) {
      best = widget;
    }
  }
  return best;
}

static bool prv_app_widget_is_active(void) {
  if (!quick_view_prefs_get_apps_enabled()) {
    return false;
  }
  mutex_lock(s_state.app_lock);
  AppPeekWidget *widget = prv_pick_app_widget();
  bool active = false;
  if (widget) {
    // Drop widgets whose app has been uninstalled.
    if (app_install_get_id_for_uuid(&widget->owner) == INSTALL_ID_INVALID) {
      widget->in_use = false;
    } else {
      active = true;
    }
  }
  mutex_unlock(s_state.app_lock);
  return active;
}

//! @return false when no live app widget remains.
static bool prv_show_app_widget(bool animated) {
  mutex_lock(s_state.app_lock);
  AppPeekWidget *widget = prv_pick_app_widget();
  if (!widget) {
    mutex_unlock(s_state.app_lock);
    return false;
  }
  const AppPeekWidget copy = *widget;
  mutex_unlock(s_state.app_lock);

  AttributeList attr_list = {};
  attribute_list_add_cstring(&attr_list, AttributeIdTitle, copy.title);
  if (copy.subtitle[0] != '\0') {
    attribute_list_add_cstring(&attr_list, AttributeIdSubtitle, copy.subtitle);
  }
  attribute_list_add_resource_id(&attr_list, AttributeIdIconTiny,
                                 copy.icon ?: TIMELINE_RESOURCE_NOTIFICATION_FLAG);

  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0 /* duration */, TimelineItemTypePin, LayoutIdGeneric, &attr_list,
      NULL /* action_group */);
  attribute_list_destroy_list(&attr_list);
  if (!item) {
    return false;
  }
  item->header.id = copy.owner;
  item->header.parent_id = copy.owner;
  timeline_peek_set_widget_item(PeekWidgetSource_App, item, animated);
  timeline_item_destroy(item);
  return true;
}

bool peek_widgets_publish_app_widget(const PeekWidgetAppPublish *publish) {
  if (!s_state.initialized || !publish || uuid_is_invalid(&publish->owner) ||
      !publish->title || (publish->title[0] == '\0')) {
    return false;
  }
  mutex_lock(s_state.app_lock);
  AppPeekWidget *slot = NULL;
  AppPeekWidget *oldest = NULL;
  for (unsigned int i = 0; i < PEEK_WIDGET_MAX_APP_WIDGETS; i++) {
    AppPeekWidget *widget = &s_state.app_widgets[i];
    if (widget->in_use && uuid_equal(&widget->owner, &publish->owner)) {
      slot = widget; // Republish replaces the app's existing widget
      break;
    }
    if (!widget->in_use) {
      slot = slot ?: widget;
    } else if (!oldest || (widget->published_at < oldest->published_at)) {
      oldest = widget;
    }
  }
  if (!slot) {
    slot = oldest; // Full: evict the oldest publish
  }
  const time_t now = rtc_get_time();
  const uint16_t timeout_s = MIN(publish->timeout_s, PEEK_WIDGET_APP_TIMEOUT_MAX_S);
  *slot = (AppPeekWidget) {
    .in_use = true,
    .owner = publish->owner,
    .icon = publish->icon,
    .launch_code = publish->launch_code,
    .published_at = now,
    .expires_at = timeout_s ? (now + timeout_s) : 0,
  };
  strncpy(slot->title, publish->title, PEEK_WIDGET_APP_TITLE_MAX_LEN);
  if (publish->subtitle) {
    strncpy(slot->subtitle, publish->subtitle, PEEK_WIDGET_APP_SUBTITLE_MAX_LEN);
  }
  mutex_unlock(s_state.app_lock);

  s_state.content_dirty = true;
  prv_request_refresh();
  return true;
}

void peek_widgets_withdraw_app_widget(const Uuid *owner) {
  if (!s_state.initialized || !owner) {
    return;
  }
  bool removed = false;
  mutex_lock(s_state.app_lock);
  for (unsigned int i = 0; i < PEEK_WIDGET_MAX_APP_WIDGETS; i++) {
    AppPeekWidget *widget = &s_state.app_widgets[i];
    if (widget->in_use && uuid_equal(&widget->owner, owner)) {
      widget->in_use = false;
      removed = true;
    }
  }
  mutex_unlock(s_state.app_lock);
  if (removed) {
    prv_request_refresh();
  }
}

// Arbitration
////////////////////////////////////////////////////////////////

static PeekWidgetSource prv_pick(void) {
  if (s_state.notif_active) {
    const time_t now = rtc_get_time();
    if (s_state.notif_expires_at && (now >= s_state.notif_expires_at)) {
      prv_notif_clear();
    } else {
      return PeekWidgetSource_Notification;
    }
  }
  if (s_state.timeline_active && timeline_peek_is_enabled()) {
    return PeekWidgetSource_Timeline;
  }
  if (prv_app_widget_is_active()) {
    return PeekWidgetSource_App;
  }
  if (prv_music_is_active()) {
    return PeekWidgetSource_Music;
  }
  return PeekWidgetSource_None;
}

//! Re-arms the wake timer for the next music/notification policy transition.
static void prv_update_timer(void) {
  const time_t now = rtc_get_time();
  time_t deadline = 0;
  if (s_state.notif_active && s_state.notif_expires_at) {
    deadline = s_state.notif_expires_at;
  }
  if ((s_state.shown == PeekWidgetSource_Music) &&
      (quick_view_prefs_get_music_mode() == QuickViewMusicMode_TrackStart)) {
    const time_t track_deadline = s_state.track_changed_at + PEEK_WIDGET_TRACK_START_SHOW_S;
    if (!deadline || (track_deadline < deadline)) {
      deadline = track_deadline;
    }
  }
  if (deadline) {
    const uint32_t timeout_ms = (deadline > now) ? ((deadline - now) * MS_PER_SECOND) : 1;
    new_timer_start(s_state.timer, timeout_ms, prv_timer_cb, NULL, 0 /* flags */);
  } else {
    new_timer_stop(s_state.timer);
  }
}

static void prv_refresh(bool animated) {
  if (!s_state.initialized) {
    return;
  }
  PBL_ASSERT_TASK(PebbleTask_KernelMain);

  PeekWidgetSource winner;
  for (;;) {
    winner = prv_pick();
    if ((winner == s_state.shown) && !s_state.content_dirty) {
      break;
    }
    bool shown = true;
    switch (winner) {
      case PeekWidgetSource_Notification:
        shown = prv_show_notification(animated);
        if (!shown) {
          prv_notif_clear();
        }
        break;
      case PeekWidgetSource_Timeline:
        shown = prv_show_timeline(animated);
        if (!shown) {
          s_state.timeline_active = false;
        }
        break;
      case PeekWidgetSource_App:
        shown = prv_show_app_widget(animated);
        // prv_show_app_widget already dropped the dead entry on failure
        break;
      case PeekWidgetSource_Music:
        prv_show_music(animated);
        break;
      case PeekWidgetSource_None:
      default:
        timeline_peek_set_item(NULL, false /* started */, 0 /* num_concurrent */,
                               false /* first */, animated);
        break;
    }
    if (shown) {
      break;
    }
    // The winner could not be displayed; re-arbitrate with it deactivated.
  }

  if (winner != s_state.shown) {
    PBL_LOG_DBG("Quick View widget source %u -> %u", s_state.shown, winner);
  }
  s_state.shown = winner;
  s_state.content_dirty = false;
  prv_update_timer();
  watchface_peek_widget_state_changed();
}

// Shell hooks
////////////////////////////////////////////////////////////////

bool peek_widgets_handle_dismiss(void) {
  switch (s_state.shown) {
    case PeekWidgetSource_Notification:
      prv_notif_clear();
      break;
    case PeekWidgetSource_Music:
      s_state.music_dismissed = true;
      s_state.music_dismissed_generation = s_state.music_generation;
      break;
    case PeekWidgetSource_App: {
      mutex_lock(s_state.app_lock);
      AppPeekWidget *widget = prv_pick_app_widget();
      if (widget) {
        widget->dismissed = true;
      }
      mutex_unlock(s_state.app_lock);
      break;
    }
    case PeekWidgetSource_Timeline:
    case PeekWidgetSource_None:
    default:
      return false;
  }
  prv_refresh(true /* animated */);
  return true;
}

bool peek_widgets_get_launch(PeekWidgetLaunch *launch_out) {
  AppInstallId app_id = INSTALL_ID_INVALID;
  uint32_t launch_code = 0;
  switch (s_state.shown) {
    case PeekWidgetSource_Music:
      app_id = APP_ID_MUSIC;
      break;
    case PeekWidgetSource_Notification:
      app_id = APP_ID_NOTIFICATIONS;
      break;
    case PeekWidgetSource_App: {
      mutex_lock(s_state.app_lock);
      AppPeekWidget *widget = prv_pick_app_widget();
      if (widget) {
        app_id = app_install_get_id_for_uuid(&widget->owner);
        launch_code = widget->launch_code;
      }
      mutex_unlock(s_state.app_lock);
      break;
    }
    case PeekWidgetSource_Timeline:
    case PeekWidgetSource_None:
    default:
      return false;
  }
  if (app_id == INSTALL_ID_INVALID) {
    return false;
  }
  if (launch_out) {
    *launch_out = (PeekWidgetLaunch) {
      .app_id = app_id,
      .launch_code = launch_code,
    };
  }
  return true;
}

void peek_widgets_handle_prefs_changed(void) {
  if (!s_state.initialized) {
    return;
  }
  prv_request_refresh();
}

void peek_widgets_init(void) {
  s_state = (PeekWidgetsState) {
    .app_lock = mutex_create(),
    .timer = new_timer_create(),
  };
  s_state.initialized = true;
}
