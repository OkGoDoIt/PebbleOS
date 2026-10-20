/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/pebble_process_md.h"

//! Ask Muse: hold a button, ask out loud, read the answer. The question goes to the receiver's
//! assistant through the audio companion service (PROTOCOL.md 4.4).
const PebbleProcessMd *muse_app_get_info(void);
