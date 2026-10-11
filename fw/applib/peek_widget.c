/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/peek_widget_private.h"

#include "syscall/syscall.h"
#include "pbl/util/math.h"

#include <string.h>

//! Copies src into dest, truncating to max_len bytes on a UTF-8 codepoint boundary.
static void prv_copy_and_truncate(char *dest, size_t max_len, const char *src) {
  const size_t src_length = src ? strlen(src) : 0;
  size_t cropped = MIN(max_len, src_length);
  if (cropped < src_length) {
    // We cut: if it landed inside a multi-byte sequence, drop the partial trailing bytes.
    while ((cropped > 0) && (((uint8_t)src[cropped] & 0xC0) == 0x80)) {
      cropped--;
    }
  }
  if (cropped) {
    memcpy(dest, src, cropped);
  }
  dest[cropped] = '\0';
}

PeekWidgetResult peek_widget_publish(const PeekWidgetInfo *info) {
  if (!info || !info->title || (info->title[0] == '\0')) {
    return PEEK_WIDGET_RESULT_INVALID_ARGS;
  }
  PeekWidgetPublishArgs args = {
    .icon = info->icon,
    .launch_code = info->launch_code,
    .timeout_s = info->timeout_s,
  };
  prv_copy_and_truncate(args.title, PEEK_WIDGET_TITLE_MAX_LEN, info->title);
  prv_copy_and_truncate(args.subtitle, PEEK_WIDGET_SUBTITLE_MAX_LEN, info->subtitle);
  return sys_peek_widget_publish(&args);
}

void peek_widget_withdraw(void) {
  sys_peek_widget_withdraw();
}
