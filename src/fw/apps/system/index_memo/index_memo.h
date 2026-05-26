/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/app_manager.h"

#define INDEX_MEMO_UUID {0x7e, 0xad, 0xb6, 0x71, 0x25, 0xb0, 0x49, 0x46, \
                         0xa4, 0x2b, 0x28, 0x33, 0x11, 0x5a, 0x04, 0x83}

const PebbleProcessMd *index_memo_get_app_info(void);
