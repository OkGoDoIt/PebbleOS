# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

import json
import os
import sys
import unittest

# Allow us to run even if not at the `tools` directory.
tools_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
sys.path.insert(0, tools_dir)

from pebble_sdk_platform import pebble_platforms


class TestSdkSchemas(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_dir = os.path.abspath(os.path.join(tools_dir, os.pardir))
        cls.schemas_dir = os.path.join(cls.repo_dir, "sdk", "tools", "schemas")

    @classmethod
    def load_json(cls, path):
        with open(path, encoding="utf-8") as json_file:
            return json.load(json_file)

    def test_schema_platforms_match_sdk_platforms(self):
        attributes = self.load_json(os.path.join(self.schemas_dir, "attributes.json"))
        schema_platforms = attributes["targetPlatforms"]["items"]["enum"]

        self.assertEqual(set(schema_platforms), set(pebble_platforms))

    def test_package_schema_accepts_every_project_type(self):
        file_types = self.load_json(os.path.join(self.schemas_dir, "file_types.json"))
        project_refs = file_types["package-json"]["properties"]["pebble"]["oneOf"]

        self.assertEqual(
            {project_ref["$ref"] for project_ref in project_refs},
            {
                "project_types.json#/native-app",
                "project_types.json#/moddable-app",
                "project_types.json#/package",
            },
        )

    def test_moddable_project_has_required_discriminator(self):
        project_types = self.load_json(os.path.join(self.schemas_dir, "project_types.json"))
        moddable_app = project_types["moddable-app"]

        self.assertEqual(moddable_app["properties"]["projectType"]["enum"], ["moddable"])
        self.assertIn("projectType", moddable_app["required"])

    def test_legacy_appinfo_accepts_moddable_projects(self):
        file_types = self.load_json(os.path.join(self.schemas_dir, "file_types.json"))
        project_types = file_types["appinfo-json"]["properties"]["projectType"]["enum"]

        self.assertIn("moddable", project_types)

    def test_default_templates_include_every_sdk_platform(self):
        for template in ("app", "lib"):
            package_json = os.path.join(
                self.repo_dir, "sdk", "defaults", template, "package.json"
            )
            project = self.load_json(package_json)
            template_platforms = project["pebble"]["targetPlatforms"]
            self.assertEqual(set(template_platforms), set(pebble_platforms))


if __name__ == "__main__":
    unittest.main()
