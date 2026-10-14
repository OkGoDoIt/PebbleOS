/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "menu.h"

#include "shell/prefs.h"

#ifdef CONFIG_SERVICE_PEEK_WIDGETS
const SettingsModuleMetadata *settings_quick_view_get_info(void);

//! The notification peek settings are shown on the Notifications page, with the duration also
//! on the Quick View page. These keep both pages identical, including which rows are shown: a
//! setting another one makes irrelevant is hidden.

//! Show As matters only when notifications can alert at all.
bool settings_quick_view_notif_show_as_visible(void);
//! The style as it behaves; no duration (a stored pref from before Peek Duration lost its
//! Disabled option) means the popup alone.
QuickViewNotifStyle settings_quick_view_notif_effective_style(void);
//! Peek If Missed matters only for full-screen popups that can time out.
bool settings_quick_view_notif_missed_visible(void);
//! Turns Peek If Missed on or off, giving the peek a duration if it has none.
void settings_quick_view_notif_missed_toggle(void);
//! The muted-peek toggle matters only when something can mute a notification: the Alerts
//! filter, or Quiet Time set to hide notifications.
bool settings_quick_view_notif_muted_visible(void);
//! The duration matters only when some notification peek can show.
bool settings_quick_view_notif_seconds_visible(void);

const char *settings_quick_view_notif_seconds_label(void);
void settings_quick_view_notif_seconds_menu_push(const char *i18n_title_key, void *i18n_owner);
//! Turns the muted peek on or off, giving it a duration if it has none.
void settings_quick_view_notif_muted_toggle(void);
#endif
