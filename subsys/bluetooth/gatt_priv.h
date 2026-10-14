/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

struct ble_gap_event;

//! Reports a peer (un)subscribing to Service Changed indications to the service layer.
void nimble_gatt_handle_subscribe_event(const struct ble_gap_event *event);
