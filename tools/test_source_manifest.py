"""Offline unit tests for fail-closed source extraction; no server or DB required."""

import collections
from pathlib import Path
import tempfile
import unittest

from build_source_manifest import MAPS, diagnose_bis, loot_paths, sql_rows


def loot(item, reference=0, group=0, mode=1, quest=0, chance=100):
    return {"Item": str(item), "Reference": str(reference), "GroupId": str(group),
            "LootMode": str(mode), "QuestRequired": str(quest), "Chance": str(chance),
            "MinCount": "1", "MaxCount": "1", "_line": 1}


class SourceManifestTests(unittest.TestCase):
    def stores(self):
        return {name: collections.defaultdict(list) for name in [
            "creature_loot_template", "gameobject_loot_template", "reference_loot_template"]}

    def test_grouped_reference_does_not_filter_child_groups(self):
        stores = self.stores()
        stores["creature_loot_template"][1] = [loot(1, reference=2, group=3)]
        stores["reference_loot_template"][2] = [loot(10, group=3), loot(20, group=4)]
        paths = list(loot_paths(stores, set(), "creature_loot_template", 1))
        self.assertEqual([item for item, chain, reasons in paths], [10, 20])
        self.assertEqual(len(paths[0][1]), 2)

    def test_saturated_group_does_not_prove_zero_chance_item(self):
        stores = self.stores()
        stores["creature_loot_template"][1] = [loot(10, chance=100, group=1), loot(20, chance=0, group=1)]
        paths = list(loot_paths(stores, set(), "creature_loot_template", 1))
        self.assertEqual(paths[0][2], [])
        self.assertIn("saturated_loot_group", paths[1][2])

    def test_cycle_and_missing_reference_fail_closed(self):
        stores = self.stores()
        stores["reference_loot_template"][1] = [loot(1, reference=2)]
        stores["reference_loot_template"][2] = [loot(1, reference=1), loot(1, reference=3)]
        self.assertEqual(list(loot_paths(stores, set(), "reference_loot_template", 1)), [])

    def test_conditions_propagate_through_reference(self):
        stores = self.stores()
        stores["creature_loot_template"][1] = [loot(1, reference=2)]
        stores["reference_loot_template"][2] = [loot(10)]
        paths = list(loot_paths(stores, {("reference_loot_template", 2)}, "creature_loot_template", 1))
        self.assertIn("loot_conditions_not_evaluated", paths[0][2])

    def test_quest_and_special_mode_remain_unverified(self):
        stores = self.stores()
        stores["creature_loot_template"][1] = [loot(10, mode=2, quest=1)]
        path = list(loot_paths(stores, set(), "creature_loot_template", 1))[0]
        self.assertEqual(set(path[2]), {"quest_required", "nondefault_loot_mode"})

    def test_zero_chance_group_entry_can_drop(self):
        stores = self.stores()
        stores["creature_loot_template"][1] = [loot(10, chance=0, group=1)]
        self.assertEqual(list(loot_paths(stores, set(), "creature_loot_template", 1))[0][2], [])

    def test_zero_chance_ungrouped_is_not_proof(self):
        stores = self.stores()
        stores["creature_loot_template"][1] = [loot(10, chance=0)]
        self.assertIn("unsupported_chance", list(loot_paths(stores, set(), "creature_loot_template", 1))[0][2])

    def test_sql_escaping_and_line_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "fixture.sql"
            path.write_text("CREATE TABLE `x` (\n  `entry` int,\n  `name` text\n);\n"
                            "INSERT INTO `x` VALUES\n(1,'Boss\\'s, chest');\n", encoding="utf-8")
            rows = sql_rows(path)
            self.assertEqual(rows[0]["name"], "Boss's, chest")
            self.assertEqual(rows[0]["_line"], 6)

    def test_unsupported_insert_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "fixture.sql"
            path.write_text("INSERT INTO `x` VALUES (1);\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                sql_rows(path)

    def test_faction_override_replaces_shared_conflict(self):
        templates = {1: {"InventoryType": "1", "FlagsExtra": "1", "AllowableRace": "-1"},
                     2: {"InventoryType": "1", "FlagsExtra": "2", "AllowableRace": "-1"}}
        result = diagnose_bis([(8, 0, 0, 0, 78, 1), (8, 0, 0, 1, 78, 2)], templates)
        self.assertEqual(result["effective_faction_conflicts"], [])

    def test_wrong_slot_is_separate_from_source(self):
        templates = {50719: {"InventoryType": "23", "FlagsExtra": "0", "AllowableRace": "-1"}}
        result = diagnose_bis([(11, 0, 17, 0, 264, 50719)], templates)
        self.assertEqual(len(result["wrong_slot_rows"]), 1)
        self.assertEqual(result["effective_faction_conflicts"], [])

    def test_catchup_maps_and_wrath_variants_are_explicit(self):
        self.assertEqual(MAPS[585][0], "TbcSunwell")
        self.assertEqual(MAPS[650][0], "WrathToc")
        self.assertEqual(MAPS[632][0], "WrathIcc")
        self.assertEqual(MAPS[533][2], [10, 25])
        self.assertNotIn(624, MAPS)


if __name__ == "__main__":
    unittest.main()
