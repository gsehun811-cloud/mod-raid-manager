"""Offline SQL/reference regressions; no server, database or character changes."""

import argparse
import hashlib
import json
from pathlib import Path
from source_context import reference_input, reference_root
import re
import unittest

from build_source_manifest import sql_rows
from test_candidate_indexes import legacy_stage_allows


MODULE = Path(__file__).resolve().parents[1]
ROOT = reference_root()


def live_bis():
    text = (ROOT / "modules/mod-playerbots/data/sql/playerbots/updates/"
            "2026_04_28_00_playerbots_bis_gear.sql").read_text(encoding="utf-8")
    raw = {}
    for line in text.splitlines():
        match = re.match(r"\s*\((\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),", line)
        if match:
            cls, tab, slot, faction, key, item = map(int, match.groups())
            raw.setdefault((key, cls, tab, faction), {})[slot] = item
    merged = {}
    for key, cls, tab in sorted({k[:3] for k in raw}):
        for faction in [1, 2]:
            values = dict(raw.get((key, cls, tab, 0), {}))
            values.update(raw.get((key, cls, tab, faction), {}))
            merged[(key, cls, tab, faction)] = values
    return raw, merged


class PreparationSets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads((MODULE / "data/preparation-sets.json").read_text())
        cls.catalog = json.loads((MODULE / "data/equipment-candidate-catalog.json").read_text())
        cls.items = cls.catalog["items"]
        cls.item_ids = {x["item"] for x in cls.items}
        cls.templates = {int(x["entry"]): x for x in sql_rows(ROOT / "data/sql/base/db_world/item_template.sql")}
        cls.raw, cls.live = live_bis()

    def test_source_fingerprints(self):
        for name, digest in self.data["inputs"].items():
            self.assertEqual(hashlib.sha256(reference_input(ROOT, name).read_bytes()).hexdigest(), digest, name)

    def test_exact_bis_references_equal_live_merge(self):
        for record in self.data["bis"]:
            expected = self.live[tuple(record[k] for k in ["key", "cls", "tab", "faction"])]
            actual = {slot: self.items[index]["item"] for slot, index in enumerate(record["indices"])
                      if record["present"] & (1 << slot)}
            self.assertEqual(actual, {s: i for s, i in expected.items() if i in self.item_ids})

    def test_faction_specific_overrides_are_not_lost(self):
        overrides = 0
        for (key, cls, tab, faction), slots in self.raw.items():
            if faction:
                for slot, item in slots.items():
                    self.assertEqual(self.live[(key, cls, tab, faction)][slot], item)
                    overrides += 1
        self.assertGreater(overrides, 0)

    def test_profiles_and_role_keys_are_unique_and_ordered(self):
        names = ["map", "difficulty", "capacity", "cls", "tab", "role", "faction"]
        keys = [tuple(row[k] for k in names) for row in self.data["sets"]]
        self.assertEqual(keys, sorted(set(keys)))
        profiles = {(x["map"], x["difficulty"], x["capacity"]) for x in self.catalog["ceilings"]}
        self.assertEqual({k[:3] for k in keys}, profiles)
        self.assertEqual(len(keys), 2304)
        self.assertTrue(any(x["cls"] == 11 and x["tab"] == 10 and x["role"] == 0 for x in self.data["sets"]))
        self.assertTrue(any(x["cls"] == 6 and x["tab"] == 0 and x["role"] == 0 for x in self.data["sets"]))

    def test_hints_fit_class_faction_progress_and_ceiling(self):
        profiles = {(p["map"], p["difficulty"], p["capacity"]): p for p in self.catalog["ceilings"]}
        for row in self.data["sets"]:
            profile = profiles[(row["map"], row["difficulty"], row["capacity"])]
            for slot, hints in enumerate(row["hints"]):
                self.assertLessEqual(len(hints), 4)
                self.assertEqual(len(hints), len(set(hints)))
                for index in hints:
                    self.assertLess(index, 65536)
                    item = self.items[index]
                    template = self.templates[item["item"]]
                    self.assertTrue(int(template["AllowableClass"]) & (1 << (row["cls"] - 1)))
                    self.assertTrue(legacy_stage_allows(item, profile["stage"]))
                    self.assertLessEqual(item["ilvl"], profile["ilvl"])
                    self.assertFalse(int(template["FlagsExtra"]) & (1 if row["faction"] == 1 else 2))
                    if row["role"] == 0 and item["class"] == 4 and slot != 14 and 1 <= item["subclass"] <= 4:
                        self.assertEqual(item["subclass"], 2 if row["cls"] == 11 else 4)
                    if row["role"] == 0 and row["cls"] in [1, 2] and slot == 16:
                        self.assertEqual(item["inventory_type"], 14)

    def test_metadata_is_not_duplicated_in_set_include(self):
        text = (MODULE / "src/RaidPreparationSetData.inc").read_text()
        self.assertNotIn("CandidateCatalogEntry", text)
        self.assertEqual(len(self.items), 23122)


def write_oracle():
    _, merged = live_bis()
    lines = ["// Independent merged SQL inputs for the production prepared-set API.",
             "struct ExpectedBis { uint16 key; uint8 cls; uint8 tab; uint8 faction; uint32 mask;",
             "    std::array<uint32, 19> items; };", "ExpectedBis const ExpectedBisRecords[] =", "{"]
    for (key, cls, tab, faction), slots in merged.items():
        mask = sum(1 << s for s in slots if s < 19)
        lines += [f"    {{{key}, {cls}, {tab}, {faction}, {mask}, {{{{",
                  "        " + ", ".join(str(slots.get(s, 0)) for s in range(10)) + ",",
                  "        " + ", ".join(str(slots.get(s, 0)) for s in range(10, 19)) + "}}},"]
    lines += ["};", ""]
    (MODULE / "tools/.build/preparation-set-oracle.inc").write_text("\n".join(lines), newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--write-oracle", action="store_true")
    args = parser.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(PreparationSets))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.write_oracle:
        write_oracle()
