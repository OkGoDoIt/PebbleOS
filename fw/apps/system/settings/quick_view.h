/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "menu.h"

#ifdef CONFIG_SERVICE_PEEK_WIDGETS
const SettingsModuleMetadata *settings_quick_view_get_info(void);

//! The notification peek duration is one setting shown on both the Notifications and the Quick
//! View settings pages; these keep the two rows identical.
const char *settings_quick_view_notif_seconds_label(void);
void settings_quick_view_notif_seconds_menu_push(const char *i18n_title_key, void *i18n_owner);
#endif
