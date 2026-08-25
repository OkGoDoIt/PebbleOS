/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! @addtogroup Foundation
//! @{
//!   @addtogroup PeekWidget Quick View Widget
//! \brief Publishing contextual widgets to the bottom of the watchface
//!
//! Quick View widgets let an app surface one line of glanceable, contextual information at the
//! bottom of the user's watchface — the same surface Timeline Quick View uses for upcoming
//! events. A widget is an icon, a title, and an optional subtitle. While it is showing, the
//! user's Down button (depending on their Quick View settings) launches the publishing app with
//! \ref APP_LAUNCH_PEEK_WIDGET as its launch reason and the widget's launch code available from
//! \ref launch_get_args(), so the app can jump straight to whatever the widget was about.
//!
//! Widgets are contextual, not persistent: publish one when the app has something timely to
//! show (a timer nearing zero, a departure coming up, a score change), give it a timeout, and
//! withdraw it when it no longer applies. The system arbitrates the surface — an upcoming
//! timeline event or a recent notification can outrank an app widget — and the user can
//! dismiss a widget with the Back button or disable app widgets entirely in Settings.
//! Widgets do not survive a reboot.
//!
//! Both the app and its worker may publish. An app has at most one widget; publishing again
//! replaces it.
//!   @{

//! The result of a \ref peek_widget_publish() call.
typedef enum PeekWidgetResult {
  //! The widget was accepted and will show when it wins the Quick View surface
  PEEK_WIDGET_RESULT_SUCCESS = 0,
  //! The widget was rejected because its info was invalid (e.g. a missing title)
  PEEK_WIDGET_RESULT_INVALID_ARGS,
  //! The user has disabled app widgets in Settings, or widgets are unavailable
  PEEK_WIDGET_RESULT_DISABLED,
} PeekWidgetResult;

//! The maximum length of a widget's title and subtitle, in bytes of UTF-8, excluding the
//! null terminator. Longer strings are truncated at a codepoint boundary.
#define PEEK_WIDGET_TITLE_MAX_LEN (32)
#define PEEK_WIDGET_SUBTITLE_MAX_LEN (32)

//! Description of an app's Quick View widget. Strings are copied during
//! \ref peek_widget_publish(), so they need not outlive the call.
typedef struct PeekWidgetInfo {
  //! The icon shown in the widget's icon box: a \ref PublishedId from the app's
  //! `publishedMedia`, or 0 for a generic icon. Tiny (25x25) icons fit best.
  uint32_t icon;
  //! The widget's title. Required.
  const char *title;
  //! The widget's subtitle, shown below the title. Optional; may be NULL.
  const char *subtitle;
  //! Handed to the app through \ref launch_get_args() when the widget's button shortcut
  //! launches it. Use it to open the relevant part of the app directly.
  uint32_t launch_code;
  //! Seconds until the widget withdraws itself, or 0 to stay until
  //! \ref peek_widget_withdraw(), a reboot, or the app is uninstalled. Capped at 12 hours.
  uint16_t timeout_s;
} PeekWidgetInfo;

//! Publishes (or replaces) this app's Quick View widget.
//! @param info Description of the widget to show
//! @return \ref PEEK_WIDGET_RESULT_SUCCESS if the widget was accepted
PeekWidgetResult peek_widget_publish(const PeekWidgetInfo *info);

//! Withdraws this app's Quick View widget, if it has one.
void peek_widget_withdraw(void);

//!   @} // group PeekWidget
//! @} // group Foundation
