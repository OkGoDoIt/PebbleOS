/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

// Installing a .pbw from the host: the bottom half reads it, from a file or
// from the page, and offers it to the firmware.

//! Start installing a .pbw, which stays the host's until released. False if
//! the firmware cannot take it yet: offer it again later.
bool samsa_pbw_install(const void *pbw, size_t len);

//! The firmware is done with a .pbw it took.
void samsa_pbw_release(void *pbw);
