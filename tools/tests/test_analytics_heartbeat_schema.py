# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

import os
import sys
import unittest


root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
repo_dir = os.path.abspath(os.path.join(root_dir, os.pardir))
sys.path.insert(0, root_dir)

from analytics_heartbeat_schema import field_offsets, native_heartbeat_layout
from analytics_heartbeat_schema import parse_analytics_def, wire_size


ANALYTICS_DEF = os.path.join(repo_dir, "include", "pbl", "services", "analytics", "analytics.def")


class TestAnalyticsHeartbeatSchema(unittest.TestCase):
    def setUp(self):
        self.metrics = parse_analytics_def(ANALYTICS_DEF)
        self.fields = native_heartbeat_layout(self.metrics)
        self.offsets = field_offsets(self.fields)

    def test_matches_official_v4_12_0_wire_layout(self):
        self.assertEqual(wire_size(self.fields), 507)
        self.assertEqual(self.offsets["version"], 0)
        self.assertEqual(self.offsets["timestamp"], 1)
        self.assertEqual(self.offsets["build_id"], 9)
        self.assertEqual(self.offsets["metric_fw_version"], 53)
        self.assertEqual(self.offsets["metric_battery_soc_pct"], 94)
        self.assertEqual(self.offsets["metric_battery_soc_pct_scale"], 98)
        self.assertEqual(self.offsets["metric_battery_soc_pct_drop"], 100)
        self.assertEqual(self.offsets["metric_battery_voltage"], 106)
        self.assertEqual(self.offsets["metric_battery_tte_s"], 118)
        self.assertEqual(self.offsets["metric_watchface_time_ms"], 314)
        self.assertEqual(self.offsets["metric_watchface_name"], 318)
        self.assertEqual(self.offsets["metric_watchface_uuid"], 351)
        self.assertEqual(self.offsets["metric_connectivity_expected_time_ms"], 503)

    def test_does_not_emit_unreleased_syscall_stack_metrics(self):
        metric_names = {metric.name for metric in self.metrics}
        self.assertNotIn("stack_free_app_syscall_bytes", metric_names)
        self.assertNotIn("stack_free_worker_syscall_bytes", metric_names)


if __name__ == "__main__":
    unittest.main()
