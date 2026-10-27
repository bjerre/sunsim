import copy
import json
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import generate_installation as generator


class InstallationDocumentTest(unittest.TestCase):
    def setUp(self):
        self.document = json.loads((ROOT / "config" / "installation.json").read_text())

    def assert_invalid(self, document, message):
        with self.assertRaisesRegex(generator.DocumentError, message):
            generator.validate_document(document)

    def test_current_installation_generates_layout_and_zones(self):
        groups, zones = generator.validate_document(self.document)

        self.assertEqual(len(groups), 5)
        self.assertEqual(len(zones), 3)
        self.assertIn("03:45", (ROOT / "config" / "installation.json").read_text())

    def test_every_light_must_belong_to_a_zone(self):
        document = copy.deepcopy(self.document)
        document["zones"].pop()

        self.assert_invalid(document, "lights missing from zones: Grow")

    def test_on_off_behavior_is_limited_to_plugs(self):
        document = copy.deepcopy(self.document)
        document["zones"][0]["lights"][0]["look"] = {"type": "on-off"}

        self.assert_invalid(document, "on-off look on a non-plug")

    def test_weather_targets_must_belong_to_their_zone(self):
        document = copy.deepcopy(self.document)
        document["zones"][0]["weather"]["precipitation"] = ["Grow"]

        self.assert_invalid(document, "must target a color light in this zone")

    def test_invalid_time_window_is_rejected(self):
        document = copy.deepcopy(self.document)
        document["zones"][1]["gate"][0]["hours"] = {"start": "19:00", "end": "08:00"}

        self.assert_invalid(document, "must be before")


if __name__ == "__main__":
    unittest.main()
