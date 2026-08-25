/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kernel/events.h"
#include "process_management/app_install_types.h"
#include "pbl/util/uuid.h"

//! Quick View widgets generalize the Timeline Quick View overlay into a contextual widget
//! surface at the bottom of the watchface. Sources (the timeline peek, now-playing music,
//! recent notifications, app-published widgets) publish their state to this arbiter, which
//! elects a single winner and drives the shared peek overlay in popups/timeline/peek.c.
//! The arbiter runs on KernelMain; every entry point below is either KernelMain-only or
//! explicitly marked safe from other tasks.

//! The source that owns the Quick View surface. Higher values outrank lower ones.
typedef enum PeekWidgetSource {
  PeekWidgetSource_None = 0,
  //! Now-playing music. Ambient, lowest priority.
  PeekWidgetSource_Music,
  //! A widget published by an installed app.
  PeekWidgetSource_App,
  //! The classic Timeline Quick View pin peek.
  PeekWidgetSource_Timeline,
  //! A recently arrived notification. Transient, highest priority.
  PeekWidgetSource_Notification,
} PeekWidgetSource;

//! Launch target for the widget currently on screen.
typedef struct PeekWidgetLaunch {
  AppInstallId app_id;
  uint32_t launch_code;
} PeekWidgetLaunch;

//! Longest app-widget title and subtitle, in bytes of UTF-8 excluding the terminator.
#define PEEK_WIDGET_APP_TITLE_MAX_LEN (32)
#define PEEK_WIDGET_APP_SUBTITLE_MAX_LEN (32)

//! Longest app-widget lifetime. Widgets published with timeout_s == 0 stay until they are
//! withdrawn, their app is uninstalled, or the watch reboots.
#define PEEK_WIDGET_APP_TIMEOUT_MAX_S (12 * 60 * 60)

//! An app-published widget. Strings are copied.
typedef struct PeekWidgetAppPublish {
  Uuid owner;             //!< The publishing app's UUID
  uint32_t icon;          //!< TimelineResourceId; 0 uses a generic fallback icon
  const char *title;      //!< Required, truncated at PEEK_WIDGET_APP_TITLE_MAX_LEN
  const char *subtitle;   //!< Optional, truncated at PEEK_WIDGET_APP_SUBTITLE_MAX_LEN
  uint32_t launch_code;   //!< Returned by launch_get_args() on a widget launch
  uint16_t timeout_s;     //!< Seconds until the widget expires; 0 = until withdrawn
} PeekWidgetAppPublish;

#ifdef CONFIG_SERVICE_PEEK_WIDGETS

//! Initializes the arbiter. Called from the shell event loop after timeline_peek_init().
void peek_widgets_init(void);

//! Consumes the timeline peek service's event in place of the peek overlay. The timeline
//! content is recorded and displayed unless another source outranks it.
void peek_widgets_handle_timeline_peek_event(PebbleTimelinePeekEvent *event);

//! Tracks now-playing changes for the music widget.
void peek_widgets_handle_media_event(PebbleMediaEvent *event);

//! Arms the notification widget when a notification arrives and clears it when the
//! notification is removed or acted upon.
void peek_widgets_handle_notification_event(PebbleSysNotificationEvent *event);

//! Starts the armed notification widget's display window when focus returns to the watchface.
void peek_widgets_handle_app_focus_event(PebbleAppFocusEvent *event);

//! Notifies the arbiter that a watchface process started. Called from the peek overlay's
//! synchronous process-start hook; defers its work to a KernelMain callback.
void peek_widgets_handle_watchface_started(void);

//! Re-evaluates the widget policies after a Quick View pref changed. Safe from any task.
void peek_widgets_handle_prefs_changed(void);

//! BACK pressed on the watchface.
//! @return true when a non-timeline widget consumed the dismissal.
bool peek_widgets_handle_dismiss(void);

//! Launch info for the widget currently on screen.
//! @return true when a non-timeline widget with a valid launch target is showing.
bool peek_widgets_get_launch(PeekWidgetLaunch *launch_out);

//! Publishes (or replaces) an app's widget. Safe from any task; the display update is
//! deferred to KernelMain.
//! @return true when the widget was accepted.
bool peek_widgets_publish_app_widget(const PeekWidgetAppPublish *publish);

//! Withdraws an app's widget, if present. Safe from any task.
void peek_widgets_withdraw_app_widget(const Uuid *owner);

#else // CONFIG_SERVICE_PEEK_WIDGETS

static inline void peek_widgets_init(void) {}
static inline void peek_widgets_handle_media_event(PebbleMediaEvent *event) {}
static inline void peek_widgets_handle_notification_event(PebbleSysNotificationEvent *event) {}
static inline void peek_widgets_handle_app_focus_event(PebbleAppFocusEvent *event) {}
static inline void peek_widgets_handle_watchface_started(void) {}
static inline void peek_widgets_handle_prefs_changed(void) {}
static inline bool peek_widgets_handle_dismiss(void) { return false; }
static inline bool peek_widgets_get_launch(PeekWidgetLaunch *launch_out) { return false; }
static inline bool peek_widgets_publish_app_widget(const PeekWidgetAppPublish *publish) {
  return false;
}
static inline void peek_widgets_withdraw_app_widget(const Uuid *owner) {}

#endif // CONFIG_SERVICE_PEEK_WIDGETS
