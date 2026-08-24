# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

import os
import sys
import unittest


root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
repo_dir = os.path.abspath(os.path.join(root_dir, os.pardir))
sys.path.insert(0, root_dir)

from analytics_heartbeat_schema import field_offsets, native_heartbeat_layout
from analytics_heartbeat_schema import parse_analytics_def, parse_native_wire_constants, wire_size
from analytics_heartbeat_schema import parse_dls_session_size_caps
from analytics_heartbeat_schema import parse_heartbeat_dls_create_buffered


ANALYTICS_DEF = os.path.join(repo_dir, "include", "pbl", "services", "analytics", "analytics.def")
NATIVE_C = os.path.join(repo_dir, "src", "fw", "services", "analytics", "native.c")
DLS_PRIVATE_H = os.path.join(repo_dir, "include", "pbl", "services", "data_logging",
                             "dls_private.h")
COMM_PROTOCOL_H = os.path.join(repo_dir, "include", "pbl", "services", "comm_session",
                               "protocol.h")

# Wire size the official backend decodes for each heartbeat record version. The version byte at
# offset 0 is its only layout selector, so these pairs are a compatibility contract, not a
# reflection of whatever the tree currently compiles to. Only extend this after confirming on a
# real watch that the backend decodes the new version -- see Sessions 29 and 91 in
# UPSTREAM_THIRD_PARTY_BACKGROUND_AUDIO_IMPLEMENTATION_PLAN.md.
BACKEND_WIRE_SIZE_BY_VERSION = {1: 527, 2: 523, 3: 563}


class TestAnalyticsHeartbeatSchema(unittest.TestCase):
    def setUp(self):
        self.metrics = parse_analytics_def(ANALYTICS_DEF)
        self.fields = native_heartbeat_layout(self.metrics)
        self.offsets = field_offsets(self.fields)

    def test_matches_official_v4_33_1_wire_layout(self):
        self.assertEqual(wire_size(self.fields), 567)
        self.assertEqual(self.offsets["version"], 0)
        self.assertEqual(self.offsets["timestamp"], 1)
        self.assertEqual(self.offsets["build_id"], 9)
        self.assertEqual(self.offsets["metric_fw_version"], 61)
        self.assertEqual(self.offsets["metric_battery_soc_pct"], 102)
        self.assertEqual(self.offsets["metric_battery_soc_pct_scale"], 106)
        self.assertEqual(self.offsets["metric_battery_soc_pct_drop"], 108)
        self.assertEqual(self.offsets["metric_battery_voltage"], 114)
        self.assertEqual(self.offsets["metric_battery_tte_s"], 126)
        self.assertEqual(self.offsets["metric_watchface_time_ms"], 326)
        self.assertEqual(self.offsets["metric_watchface_name"], 330)
        self.assertEqual(self.offsets["metric_watchface_uuid"], 363)
        self.assertEqual(self.offsets["metric_ppog_reversed"], 467)
        self.assertEqual(self.offsets["metric_settings_touch_enabled"], 499)
        self.assertEqual(self.offsets["metric_connectivity_expected_time_ms"], 519)
        self.assertEqual(self.offsets["metric_ble_conn_slave_lat0_time_ms"], 523)
        self.assertEqual(self.offsets["metric_battery_soc_pct_min"], 557)
        self.assertEqual(self.offsets["metric_touch_gated_touchdown_count"], 563)

    def test_transmitted_size_matches_the_emitted_version_byte(self):
        """The backend picks a layout purely from the version byte, so the two must agree.

        Upstream may append metrics without bumping the version byte (v4.33 did, 563 -> 567).
        That is fine as long as we keep transmitting the prefix the version byte promises; it is
        not fine to raise the transmitted size to match, which is what broke the Battery screen.
        """
        constants = parse_native_wire_constants(NATIVE_C)
        version = constants["NATIVE_HEARTBEAT_RECORD_VERSION"]
        transmitted = constants["NATIVE_HEARTBEAT_TRANSMIT_SIZE"]

        self.assertIn(
            version,
            BACKEND_WIRE_SIZE_BY_VERSION,
            f"heartbeat record version {version} has no known backend wire size; confirm what the "
            f"official backend decodes for it before shipping",
        )
        self.assertEqual(
            transmitted,
            BACKEND_WIRE_SIZE_BY_VERSION[version],
            f"transmitting {transmitted} bytes while advertising version {version} "
            f"({BACKEND_WIRE_SIZE_BY_VERSION[version]} bytes): the backend will decode batched "
            f"records shifted -- 0% battery and a garbled watchface name",
        )

    def test_truncated_tail_is_only_post_v3_appends(self):
        """Everything outside the transmitted prefix must be metrics appended after version 3."""
        constants = parse_native_wire_constants(NATIVE_C)
        transmitted = constants["NATIVE_HEARTBEAT_TRANSMIT_SIZE"]

        self.assertLessEqual(transmitted, wire_size(self.fields))
        self.assertIn(
            transmitted,
            {field.offset for field in self.fields},
            "the transmitted prefix must end on a field boundary, or the backend reads a "
            "half-written metric",
        )
        self.assertEqual(self.offsets["metric_touch_gated_touchdown_count"], transmitted)

    def test_session_type_can_carry_the_record(self):
        """The heartbeat item must fit the caps of the DLS session type native.c requests.

        Buffered sessions cap items at DLS_SESSION_MAX_BUFFERED_ITEM_SIZE (300 bytes) -- far
        below the heartbeat record -- so dls_create returns NULL and the PBL_ASSERTN right after
        it reboots the watch on every heartbeat. Upstream's v4.35 `buffered=true` change
        (bebc13477) trips exactly this and must not be taken on a merge. Unbuffered sessions cap
        items at the comm outbound payload limit instead.
        """
        buffered = parse_heartbeat_dls_create_buffered(NATIVE_C)
        max_buffered, max_unbuffered = parse_dls_session_size_caps(DLS_PRIVATE_H, COMM_PROTOCOL_H)
        constants = parse_native_wire_constants(NATIVE_C)
        transmitted = constants["NATIVE_HEARTBEAT_TRANSMIT_SIZE"]

        cap = max_buffered if buffered else max_unbuffered
        self.assertLessEqual(
            transmitted,
            cap,
            f"heartbeat dls_create({'buffered' if buffered else 'unbuffered'}) caps items at "
            f"{cap} bytes but the record is {transmitted}: dls_create returns NULL and the "
            f"assert after it reboots the watch every heartbeat",
        )

    def test_emits_released_syscall_stack_metrics(self):
        metric_names = {metric.name for metric in self.metrics}
        self.assertIn("stack_free_app_syscall_bytes", metric_names)
        self.assertIn("stack_free_worker_syscall_bytes", metric_names)
        self.assertIn("ppog_reversed", metric_names)
        self.assertIn("settings_touch_enabled", metric_names)


if __name__ == "__main__":
    unittest.main()
