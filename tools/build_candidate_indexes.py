"""Generate ordered stage/class/slot views with uint16 references to one catalog.

The legacy pool is retained for snapshot mismatch fallback. Unknown acquisition
remains unknown; these are eligibility views, not inferred introduction eras.
"""

import argparse
import hashlib
import json
from pathlib import Path
from source_context import input_name, reference_root
import re
import struct

from build_candidate_catalog import SLOTS, STAGES
from build_source_manifest import sql_rows


CLASSES = [1, 2, 3, 4, 5, 6, 7, 8, 9, 11]
CLASS_MASK = sum(1 << (c - 1) for c in CLASSES)
RACE_MASK = sum(1 << (r - 1) for r in [1, 2, 3, 4, 5, 6, 7, 8, 10, 11])
INTERNAL_NAMES = ["NPC ", "Monster ", "Test", "Deprecated ", "[PH]", "TEST", "Unused ", "zzOLD",
                  "(test)", "(OLD)", "QR", "2200 "]


def read_skill_mapping(root):
    header = (root / "src/server/game/Entities/Item/ItemTemplate.h").read_text(encoding="utf-8")
    shared = (root / "src/server/shared/SharedDefines.h").read_text(encoding="utf-8")
    values = {key: int(value) for key, value in re.findall(r"\b(SKILL_\w+)\s*=\s*(\d+)", shared)}
    mappings = []
    for name in ["item_weapon_skills", "item_armor_skills"]:
        body = re.search(name + r"\[[^]]+\]\s*=\s*\{([^}]+)\}", header, re.S)[1]
        mappings.append([values[token] if token != "0" else 0
                         for token in re.findall(r"\bSKILL_\w+\b|\b0\b", body)])
    return mappings


def seed_from_dbc(path):
    raw = path.read_bytes()
    magic, count, fields, size, strings = struct.unpack("<4s4I", raw[:20])
    if magic != b"WDBC" or fields != 8 or size != 32 or len(raw) != 20 + count * size + strings:
        raise ValueError("Unsupported SkillRaceClassInfo.dbc layout")
    masks = {}
    for index in range(count):
        row = struct.unpack_from("<8I", raw, 20 + index * size)
        if not row[2] or row[2] & RACE_MASK:
            masks[row[1]] = masks.get(row[1], 0) | ((row[3] or CLASS_MASK) & CLASS_MASK)
    return {"schema": 1, "source": "SkillRaceClassInfo.dbc", "sha256": hashlib.sha256(raw).hexdigest(),
            "meaning": "Possible class proficiency across playable races, not currently learned skills",
            "skills": [{"skill": skill, "class_mask": mask} for skill, mask in sorted(masks.items())]}


def unsupported(template):
    return bool(int(template["RandomProperty"]) or int(template["RandomSuffix"]) or
                int(template["area"]) or int(template["Map"]) or int(template["requiredhonorrank"]) or
                int(template["RequiredCityRank"]) or int(template["Flags"]) & 16 or
                not int(template["AllowableClass"]) & CLASS_MASK or
                not int(template["AllowableRace"]) & RACE_MASK or
                any(token in template["name"] for token in INTERNAL_NAMES))


def stage_allowed(item, stage):
    level = 60 if stage < 7 else 70 if stage < 13 else 80
    source = item["observed_source"]
    return bool(item["ilvl"]) and item["required_level"] <= level and not (
        level <= 60 and item["item"] >= 23728 or level <= 70 and item["item"] >= 35570) and (
        not source or source["stage"] <= stage)


def class_allowed(item, template, cls, skill_masks, mappings):
    bit = 1 << (cls - 1)
    if not int(template["AllowableClass"]) & bit:
        return False
    mapping = mappings[0 if item["class"] == 2 else 1]
    skill = mapping[item["subclass"]] if item["subclass"] < len(mapping) else 0
    # No inference for unknown proficiency IDs; keep the broad candidate.
    if skill and skill in skill_masks and not skill_masks[skill] & bit:
        return False
    relics = {7: 2, 8: 11, 9: 7, 10: 6}
    return not (item["inventory_type"] == 28 and item["subclass"] in relics and relics[item["subclass"]] != cls)


def build(root, catalog, seed):
    if len(catalog["items"]) > 65536:
        raise ValueError("uint16 catalog index overflow")
    template_path = root / "data/sql/base/db_world/item_template.sql"
    templates = {int(row["entry"]): row for row in sql_rows(template_path)}
    override_path = root / "data/sql/base/db_world/skillraceclassinfo_dbc.sql"
    if "INSERT INTO" in override_path.read_text(encoding="utf-8"):
        raise ValueError("Review class proficiency seed against skillraceclassinfo SQL overrides first")
    mappings = read_skill_mapping(root)
    skills = {row["skill"]: row["class_mask"] for row in seed["skills"]}
    class_masks = []
    rejected = []
    raw_masks = []
    for item in catalog["items"]:
        template = templates[item["item"]]
        class_masks.append(sum(1 << (cls - 1) for cls in CLASSES
                               if class_allowed(item, template, cls, skills, mappings)))
        rejected.append(unsupported(template))
        raw_masks.append(int(template["AllowableClass"]) & CLASS_MASK)
    pools = []
    for slot in range(19):
        values = [i for i, item in enumerate(catalog["items"]) if slot in SLOTS[item["inventory_type"]]]
        values.sort(key=lambda i: (-catalog["items"][i]["ilvl"], catalog["items"][i]["item"]))
        pools.append(values)
    indices = []
    offsets = [0]
    for stage in range(1, len(STAGES)):
        stage_ok = [stage_allowed(item, stage) for item in catalog["items"]]
        for cls in CLASSES:
            for pool in pools:
                indices.extend(i for i in pool if stage_ok[i] and class_masks[i] & (1 << (cls - 1)) and
                               not rejected[i])
                offsets.append(len(indices))
    tracked = sorted(set(mappings[0] + mappings[1]) - {0})
    tracked_masks = [skills.get(skill, CLASS_MASK) for skill in tracked]
    return {"indices": indices, "offsets": offsets, "raw_masks": raw_masks, "rejected": rejected,
            "class_masks": class_masks, "skills": tracked, "skill_masks": tracked_masks}


def render_array(kind, name, values, width=16):
    result = [f"{kind} const {name}[] =", "{"]
    for start in range(0, len(values), width):
        result.append("    " + ", ".join(map(str, values[start:start + width])) + ",")
    return result + ["};"]


def render(result):
    lines = ["// Generated by tools/build_candidate_indexes.py; all references address CandidateCatalog."]
    for kind, name, values in [
        ("uint16", "StageClassPoolIndices", result["indices"]),
        ("uint32", "StageClassPoolOffsets", result["offsets"]),
        ("uint16", "CandidateSnapshotClassMasks", result["raw_masks"]),
        ("uint8", "CandidateSnapshotUnsupported", [int(v) for v in result["rejected"]]),
        ("uint32", "CandidateProficiencySkills", result["skills"]),
        ("uint16", "CandidateProficiencyClassMasks", result["skill_masks"]),
    ]:
        lines.extend(render_array(kind, name, values, 12 if kind == "uint32" else 16))
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--skill-dbc", type=Path, help="Explicitly import a trusted proficiency snapshot")
    args = parser.parse_args()
    module = Path(__file__).resolve().parents[1]
    root = reference_root()
    seed_path = module / "data/equipment-class-proficiencies.json"
    seed = seed_from_dbc(args.skill_dbc) if args.skill_dbc else json.loads(seed_path.read_text(encoding="utf-8"))
    catalog_path = module / "data/equipment-candidate-catalog.json"
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    result = build(root, catalog, seed)
    inputs = [catalog_path, root / "data/sql/base/db_world/item_template.sql",
              root / "data/sql/base/db_world/skillraceclassinfo_dbc.sql",
              root / "src/server/game/Entities/Item/ItemTemplate.h", root / "src/server/shared/SharedDefines.h",
              root / "modules/mod-playerbots/src/Mgr/Item/RandomItemMgr.cpp"]
    summary = {"schema": 1, "stages": 18, "classes": CLASSES, "slots": 19, "index_bits": 16,
               "catalog_count": len(catalog["items"]), "reference_count": len(result["indices"]),
               "directory_count": len(result["offsets"]), "unsupported_templates": sum(result["rejected"]),
               "proficiency_sha256": seed["sha256"], "reference_bytes": len(result["indices"]) * 2,
               "inputs_sha256": {input_name(p, root, module):
                                 hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}}
    outputs = {module / "src/RaidEquipmentCandidateIndexes.inc": render(result),
               module / "data/equipment-candidate-indexes.json": json.dumps(summary, indent=2) + "\n"}
    if args.skill_dbc:
        outputs[seed_path] = json.dumps(seed, indent=2) + "\n"
    for path, text in outputs.items():
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                raise SystemExit(f"Stale index data: {path}")
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
    print(json.dumps({k: v for k, v in summary.items() if k != "inputs_sha256"}))


if __name__ == "__main__":
    main()
