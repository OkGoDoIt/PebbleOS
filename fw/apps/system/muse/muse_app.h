/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/pebble_process_md.h"

//! Ask Muse: hold a button, ask out loud, read the answer. The question goes to the receiver's
//! assistant through the audio companion service (PROTOCOL.md 4.4).
const PebbleProcessMd *muse_app_get_info(void);

//! The Muse app's UUID: 3b8e6f0a-41c2-4d7e-9a35-c0f1e2d4b6a8. Its notifications name it as their
//! parent, so their actions open it.
#define MUSE_APP_UUID                                                                        \
  {0x3b, 0x8e, 0x6f, 0x0a, 0x41, 0xc2, 0x4d, 0x7e, 0x9a, 0x35, 0xc0, 0xf1, 0xe2, 0xd4, 0xb6, \
   0xa8}
