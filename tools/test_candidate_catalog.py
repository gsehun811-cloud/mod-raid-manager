"""Offline integration checks for the generated catalog and its recorded evidence."""

import hashlib
import json
from pathlib import Path
from source_context import reference_root
import re
import unittest

from build_candidate_catalog import MAPS, SLOTS, STAGES


class CandidateCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = Path(__file__).resolve().parents[1]
        cls.root = reference_root()
        cls.data = json.loads((cls.module / "data/equipment-candidate-catalog.json").read_text(encoding="utf-8"))
        cls.items = {r["item"]: r for r in cls.data["items"]}

    def test_input_hashes_match_sql_and_scripts(self):
        for name, digest in self.data["inputs_sha256"].items():
            self.assertEqual(hashlib.sha256((self.root / name).read_bytes()).hexdigest(), digest, name)

    def test_catalog_is_unique_and_sorted(self):
        ids = [r["item"] for r in self.data["items"]]
        self.assertEqual(ids, sorted(set(ids)))
        self.assertGreater(len(ids), 20000)

    def test_ceilings_have_actual_item_anchors_and_capacity(self):
        profiles = set()
        for ceiling in self.data["ceilings"]:
            key = ceiling["map"], ceiling["difficulty"]
            self.assertNotIn(key, profiles)
            profiles.add(key)
            self.assertEqual(self.items[ceiling["item"]]["ilvl"], ceiling["ilvl"])
            self.assertEqual(MAPS[ceiling["map"]][2][ceiling["difficulty"]], ceiling["capacity"])
            self.assertTrue(ceiling["loot_path"])

    def test_cpp_stage_order_matches_generator(self):
        header = (self.module / "src/RaidEquipmentSources.h").read_text(encoding="utf-8")
        body = header.split("enum class UnlockStage", 1)[1].split("{", 1)[1].split("}", 1)[0]
        self.assertEqual(re.findall(r"\b[A-Za-z][A-Za-z0-9_]*\b", body), STAGES)

    def test_later_icc_source_contaminating_uldar_is_recorded(self):
        source = self.items[51842]["observed_source"]
        self.assertEqual(source["map"], 631)
        self.assertEqual(source["stage"], STAGES.index("WrathIcc"))

    def test_unknown_sources_remain_unknown(self):
        unknown = [r for r in self.data["items"] if r["observed_source"] is None]
        self.assertTrue(unknown)
        self.assertTrue(all(r["verified_dungeon"] is None for r in unknown))

    def test_all_template_inventory_types_have_explicit_slot_mapping(self):
        for item in self.data["items"]:
            self.assertIn(item["inventory_type"], SLOTS)
            self.assertTrue(all(slot not in [3, 18] for slot in SLOTS[item["inventory_type"]]))


if __name__ == "__main__":
    unittest.main()
