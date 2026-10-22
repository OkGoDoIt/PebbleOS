/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/pebble_process_md.h"

//! Mark Moment: a Quick Launch action that marks now as important in the recording. The mark
//! goes to the receiver through the audio companion service (PROTOCOL.md 4.6).
const PebbleProcessMd *mark_moment_get_info(void);
