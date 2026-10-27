"""Regression oracle from the legacy catalog and fixed core equip rejection rules.

No server/DB access. Optional native fixtures contain admission masks, not copied
equipment metadata. The original canonical data and safety functions are pinned.
"""

import argparse
import functools
import hashlib
import json
from pathlib import Path
from source_context import reference_root
import re
import unittest

from build_candidate_catalog import SLOTS, STAGES
from build_candidate_indexes import CLASSES, CLASS_MASK, RACE_MASK, build, class_allowed, unsupported
from build_source_manifest import sql_rows


@functools.lru_cache(maxsize=1)
def context():
    module = Path(__file__).resolve().parents[1]
    root = reference_root()
    catalog = json.loads((module / "data/equipment-candidate-catalog.json").read_text(encoding="utf-8"))
    seed = json.loads((module / "data/equipment-class-proficiencies.json").read_text(encoding="utf-8"))
    result = build(root, catalog, seed)
    templates = {int(r["entry"]): r for r in sql_rows(root / "data/sql/base/db_world/item_template.sql")}
    skills = {r["skill"]: r["class_mask"] for r in seed["skills"]}
    weapon = [44, 172, 45, 46, 54, 160, 229, 43, 55, 0, 136, 0, 0, 473, 0, 173, 176, 253, 226, 228, 356]
    armor = [0, 415, 414, 413, 293, 0, 433, 0, 0, 0, 0]
    # Independent oracle: model the legacy core's class mask, proficiency and relic checks.
    # Missing proficiency records remain broad to avoid inferring a rejection.
    masks = []
    rejected = []
    for item in catalog["items"]:
        t = templates[item["item"]]
        invalid = bool(int(t["RandomProperty"]) or int(t["RandomSuffix"]) or int(t["area"]) or int(t["Map"]) or
                       int(t["requiredhonorrank"]) or int(t["RequiredCityRank"]) or int(t["Flags"]) & 16 or
                       not int(t["AllowableClass"]) & CLASS_MASK or not int(t["AllowableRace"]) & RACE_MASK or
                       any(word in t["name"] for word in ["NPC ", "Monster ", "Test", "Deprecated ", "[PH]",
                           "TEST", "Unused ", "zzOLD", "(test)", "(OLD)", "QR", "2200 "]))
        rejected.append(invalid)
        m = 0
        for cls in CLASSES:
            bit = 1 << (cls - 1)
            if invalid or not int(t["AllowableClass"]) & bit:
                continue
            table = weapon if item["class"] == 2 else armor
            skill = table[item["subclass"]] if item["subclass"] < len(table) else 0
            if skill and not skills.get(skill, CLASS_MASK) & bit:
                continue
            if item["inventory_type"] == 28:
                owner = {7: 2, 8: 11, 9: 7, 10: 6}.get(item["subclass"])
                if owner is not None and cls != owner:
                    continue
            m |= bit
        masks.append(m)
    return module, root, catalog, result, templates, masks, rejected


def legacy_stage_allows(item, stage):
    level = 60 if stage < 7 else 70 if stage < 13 else 80
    return (item["ilvl"] > 0 and item["required_level"] <= level and
            not (level <= 60 and item["item"] >= 23728) and
            not (level <= 70 and item["item"] >= 35570) and
            (not item["observed_source"] or item["observed_source"]["stage"] <= stage))


class CandidateIndexRegression(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module, cls.root, cls.catalog, cls.result, cls.templates, cls.masks, cls.rejected = context()

    def test_original_catalog_and_ceilings_unchanged(self):
        baseline = json.loads((self.module / "data/candidate-index-regression-baseline.json").read_text())
        for key in ["items", "ceilings"]:
            raw = json.dumps(self.catalog[key], sort_keys=True, separators=(",", ":")).encode()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), baseline[key], key)

    def test_original_safety_and_inventory_code_unchanged(self):
        baseline = json.loads((self.module / "data/candidate-index-regression-baseline.json").read_text())
        for name, digest in baseline["files"].items():
            if name == "RaidWork.cpp":
                continue  # Authorized scheduler split is covered by preparation behavior tests.
            self.assertEqual(hashlib.sha256((self.module / "src" / name).read_bytes()).hexdigest(), digest, name)
        for key, digest in baseline["functions"].items():
            if key == "RaidPreparation.cpp:LearnStep":
                continue  # Authorized bounded learning batch; learning safety helper remains pinned separately.
            name, function = key.split(":")
            text = (self.module / "src" / name).read_text(encoding="utf-8")
            match = re.search(r"\b" + function + r"\([^;]*?\)\s*\{", text, re.S)
            start = match.end() - 1
            end = start + 1
            depth = 1
            while depth:
                depth += (text[end] == "{") - (text[end] == "}")
                end += 1
            body = re.sub(r"\s+", " ", text[start:end]).strip()
            self.assertEqual(hashlib.sha256(body.encode()).hexdigest(), digest, key)

    def test_all_3420_buckets_equal_filtered_legacy_order(self):
        items = self.catalog["items"]
        legacy = []
        for slot in range(19):
            pool = [i for i, item in enumerate(items) if slot in SLOTS[item["inventory_type"]]]
            pool.sort(key=lambda i: (-items[i]["ilvl"], items[i]["item"]))
            legacy.append(pool)
        bucket = 0
        for stage in range(1, 19):
            stage_ok = [legacy_stage_allows(item, stage) for item in items]
            for cls in CLASSES:
                for pool in legacy:
                    expected = [i for i in pool if stage_ok[i] and self.masks[i] & (1 << (cls - 1))]
                    first, last = self.result["offsets"][bucket:bucket + 2]
                    self.assertEqual(self.result["indices"][first:last], expected, (stage, cls, bucket % 19))
                    bucket += 1

    def test_previous_era_eligibility_is_cumulative(self):
        offsets = self.result["offsets"]
        for cls in range(10):
            for slot in range(19):
                sets = []
                for stage in [6, 12, 18]:
                    bucket = ((stage - 1) * 10 + cls) * 19 + slot
                    sets.append(set(self.result["indices"][offsets[bucket]:offsets[bucket + 1]]))
                self.assertTrue(sets[0] <= sets[1] <= sets[2])

    def test_shared_cloth_and_class_restrictions(self):
        item = {"class": 4, "subclass": 1, "inventory_type": 5}
        skills = {415: CLASS_MASK}
        maps = [[], [0, 415]]
        shared = {"AllowableClass": "-1"}
        for cls in CLASSES:
            self.assertTrue(class_allowed(item, shared, cls, skills, maps))
        restricted = {"AllowableClass": str(1 << (8 - 1))}
        self.assertTrue(class_allowed(item, restricted, 8, skills, maps))
        self.assertFalse(class_allowed(item, restricted, 1, skills, maps))

    def test_only_fixed_rejection_conditions_are_removed(self):
        base = dict(next(iter(self.templates.values())), name="Ordinary gear", Flags="0", RandomProperty="0",
                    RandomSuffix="0", area="0", Map="0", requiredhonorrank="0", RequiredCityRank="0",
                    AllowableClass="-1", AllowableRace="-1")
        self.assertFalse(unsupported(base))
        for key in ["RandomProperty", "RandomSuffix", "area", "Map", "requiredhonorrank", "RequiredCityRank"]:
            self.assertTrue(unsupported(dict(base, **{key: "1"})), key)
        for key in ["RequiredSkill", "RequiredReputationFaction", "requiredspell"]:
            self.assertFalse(unsupported(dict(base, **{key: "999"})), key)
        self.assertTrue(unsupported(dict(base, name="Monster sword")))
        self.assertTrue(unsupported(dict(base, Flags="16")))

    def test_uint16_reference_bounds_and_single_metadata_table(self):
        self.assertLessEqual(len(self.catalog["items"]), 65536)
        self.assertTrue(all(0 <= index < len(self.catalog["items"]) for index in self.result["indices"]))
        self.assertEqual(len(self.result["offsets"]), 3421)
        text = (self.module / "src/RaidEquipmentCandidateIndexes.inc").read_text(encoding="utf-8")
        self.assertIn("uint16 const StageClassPoolIndices[]", text)
        self.assertNotIn("CandidateCatalogEntry", text)

    def test_unknown_sources_are_not_misclassified(self):
        item = next(r for r in self.catalog["items"] if not r["observed_source"] and
                    r["item"] < 23728 and r["required_level"] <= 60)
        self.assertTrue(all(legacy_stage_allows(item, s) for s in [2, 6, 8, 12, 14, 18]))
        self.assertIsNone(item["observed_source"])

    def test_overflow_is_rejected_before_generation(self):
        from build_candidate_indexes import build
        with self.assertRaises(ValueError):
            build(self.root, {"items": [None] * 65537}, {})


def write_native_oracle():
    module, root, catalog, result, templates, masks, rejected = context()
    directory = module / "tools/.build"
    directory.mkdir(exist_ok=True)
    lines = ["// Test-only oracle from legacy order and independent fixed core admission predicates."]
    for kind, name, values in [
        ("uint16", "IndexOracleMasks", masks),
        ("uint16", "IndexOracleRawMasks", [int(templates[r["item"]]["AllowableClass"]) & CLASS_MASK
                                           for r in catalog["items"]]),
        ("uint8", "IndexOracleUnsupported", [int(v) for v in rejected]),
    ]:
        lines += [f"{kind} const {name}[] =", "{"]
        for start in range(0, len(values), 16):
            lines.append("    " + ", ".join(map(str, values[start:start + 16])) + ",")
        lines.append("};")
    lines += ["struct OracleProfile { uint32 map; uint32 difficulty; uint32 capacity; };",
              "OracleProfile const OracleProfiles[] =", "{"]
    for row in catalog["ceilings"]:
        lines.append(f"    {{{row['map']}, {row['difficulty']}, {row['capacity']}}},")
    lines += ["};", ""]
    (directory / "candidate-index-oracle.inc").write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-oracle", action="store_true")
    args = parser.parse_args()
    outcome = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(
        CandidateIndexRegression))
    if not outcome.wasSuccessful():
        raise SystemExit(1)
    if args.write_oracle:
        write_native_oracle()
