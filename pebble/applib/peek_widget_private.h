/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "applib/peek_widget.h"

//! Wire form of a widget publish, passed from the applib wrapper to the syscall. Strings are
//! copied into fixed buffers app-side so the kernel validates exactly one buffer.
typedef struct PeekWidgetPublishArgs {
  uint32_t icon;
  char title[PEEK_WIDGET_TITLE_MAX_LEN + 1];
  char subtitle[PEEK_WIDGET_SUBTITLE_MAX_LEN + 1];
  uint32_t launch_code;
  uint16_t timeout_s;
} PeekWidgetPublishArgs;
