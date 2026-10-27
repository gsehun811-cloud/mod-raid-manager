"""Offline fallback catalog, observed source stages, and loot-derived ilvl ceilings.

No database access. Unknown acquisition is deliberately different from a verified
source. Era item-ID guards are conservative exclusions, never source proof.
"""

import argparse
import collections
import functools
import hashlib
import json
from pathlib import Path
from source_context import reference_root

from build_source_manifest import MAPS, loot_paths, sql_rows


STAGES = ["Unknown", "ClassicDungeons", "ClassicMC", "ClassicBWL", "ClassicZG", "ClassicAQ", "ClassicNaxx",
          "TbcDungeons", "TbcT4", "TbcT5", "TbcT6", "TbcZA", "TbcSunwell", "WrathDungeons", "WrathT7",
          "WrathUlduar", "WrathToc", "WrathIcc", "WrathRuby"]
SLOTS = {1: [0], 2: [1], 3: [2], 5: [4], 20: [4], 6: [5], 7: [6], 8: [7], 9: [8], 10: [9],
         11: [10, 11], 12: [12, 13], 16: [14], 13: [15, 16], 17: [15, 16], 21: [15],
         22: [16], 14: [16], 23: [16], 15: [17], 25: [17], 26: [17], 28: [17]}

# Explicit script-bound bosses supplement absent SQL spawns for ceiling/known-stage evidence only.
# Their loot is not promoted to a verified automatically obtainable item source.
SCRIPT_BOSSES = [
    (409, 11502, "EasternKingdoms/BlackrockMountain/MoltenCore/molten_core.h"),
    (469, 11583, "EasternKingdoms/BlackrockMountain/BlackwingLair/blackwing_lair.h"),
    (249, 10184, "Kalimdor/OnyxiasLair/onyxias_lair.h"),
    (544, 17257, "Outland/HellfireCitadel/MagtheridonsLair/magtheridons_lair.h"),
    (568, 23863, "EasternKingdoms/ZulAman/zulaman.h"),
    (580, 25315, "EasternKingdoms/SunwellPlateau/sunwell_plateau.h"),
    (649, 34564, "Northrend/CrusadersColiseum/TrialOfTheCrusader/trial_of_the_crusader.h"),
]


def generate(root):
    inputs = {}

    def read(table):
        path = root / f"data/sql/base/db_world/{table}.sql"
        inputs[str(path.relative_to(root)).replace("\\", "/")] = hashlib.sha256(path.read_bytes()).hexdigest()
        return sql_rows(path)

    items = {int(r["entry"]): r for r in read("item_template")}
    # Use exact SQL column spelling, and exclude temporary/holiday/scaling items.
    eligible = {i: r for i, r in items.items() if int(r["class"]) in [2, 4] and
                int(r["InventoryType"]) in SLOTS and 2 <= int(r["Quality"]) <= 4 and
                not int(r["duration"]) and not int(r["HolidayId"]) and not int(r["ScalingStatDistribution"])}
    stores = {}
    for table in ["creature_loot_template", "gameobject_loot_template", "reference_loot_template"]:
        stores[table] = collections.defaultdict(list)
        for row in read(table):
            stores[table][int(row["Entry"])].append(row)
    conditions = set()
    kinds = {1: "creature_loot_template", 4: "gameobject_loot_template", 10: "reference_loot_template"}
    for row in read("conditions"):
        if int(row["SourceTypeOrReferenceId"]) in kinds:
            conditions.add((kinds[int(row["SourceTypeOrReferenceId"])], int(row["SourceGroup"])))
    creatures = {int(r["entry"]): r for r in read("creature_template")}
    objects = {int(r["entry"]): r for r in read("gameobject_template")}
    events = {"creature": {int(r["guid"]) for r in read("game_event_creature")},
              "gameobject": {int(r["guid"]) for r in read("game_event_gameobject")}}

    @functools.lru_cache(maxsize=None)
    def paths(table, entry):
        return list(loot_paths(stores, conditions, table, entry))

    observed = {}
    dungeon = {}
    caps = {}
    visited = set()

    def visit(spawn, template, map_id, mode, table, entry, kind):
        restricted = int(spawn["phaseMask"]) != 1 or int(spawn["guid"]) in events[kind]
        if kind == "gameobject":
            restricted |= any(int(template[f"Data{i}"]) for i in [0, 6, 8])
        key = map_id, mode, table, entry, restricted
        if key in visited:
            return
        visited.add(key)
        stage, raid, capacities = MAPS[map_id]
        stage_id = STAGES.index(stage)
        for item, chain, reasons in paths(table, entry):
            if item not in eligible:
                continue
            evidence = {"map": map_id, "difficulty": mode, "stage": stage_id, "raid": raid,
                        "spawn_table": kind, "spawn_line": spawn["_line"], "spawn_guid": int(spawn["guid"]),
                        "template_line": template["_line"], "loot_path": chain}
            if "script_basis" in spawn:
                evidence["script_basis"] = spawn["script_basis"]
            if item not in observed or (stage_id, mode) < (observed[item]["stage"], observed[item]["difficulty"]):
                observed[item] = evidence
            era_level = 60 if stage_id < STAGES.index("TbcDungeons") else 70 if stage_id < 13 else 80
            if int(eligible[item]["RequiredLevel"]) > era_level:
                continue
            if (era_level == 60 and item >= 23728) or (era_level == 70 and item >= 35570):
                continue
            # A ceiling is an upper bound, not proof that a character meets loot conditions.
            # Keep conditional boss drops as cap evidence, but never special hard-mode loot.
            invalid_cap = {"nondefault_loot_mode", "invalid_count", "unsupported_chance", "saturated_loot_group"}
            if raid and not invalid_cap.intersection(reasons):
                ilvl = int(eligible[item]["ItemLevel"])
                key = map_id, mode
                if key not in caps or ilvl > caps[key]["ilvl"]:
                    caps[key] = dict(evidence, ilvl=ilvl, item=item, capacity=capacities[mode],
                                     unresolved_conditions=reasons, spawn_restricted=bool(restricted))
            if restricted or reasons:
                continue
            if not raid and (item not in dungeon or stage_id < dungeon[item]["stage"]):
                dungeon[item] = evidence

    for spawn in read("creature"):
        map_id = int(spawn["map"])
        if map_id not in MAPS:
            continue
        for field in ["id1", "id2", "id3"]:
            normal = creatures.get(int(spawn[field]))
            if not normal:
                continue
            for mode in range(len(MAPS[map_id][2])):
                entry = int(normal[f"difficulty_entry_{mode}"]) if mode else int(normal["entry"])
                template = creatures.get(entry)
                if template and int(spawn["spawnMask"]) & (1 << mode) and int(template["lootid"]):
                    visit(spawn, template, map_id, mode, "creature_loot_template", int(template["lootid"]), "creature")
    for spawn in read("gameobject"):
        map_id = int(spawn["map"])
        template = objects.get(int(spawn["id"]))
        if map_id not in MAPS or not template or int(template["type"]) != 3:
            continue
        for mode in range(len(MAPS[map_id][2])):
            if int(spawn["spawnMask"]) & (1 << mode) and int(template["Data1"]):
                visit(spawn, template, map_id, mode, "gameobject_loot_template", int(template["Data1"]), "gameobject")
    for map_id, boss, header in SCRIPT_BOSSES:
        script = root / "src/server/scripts" / header
        content = script.read_text(encoding="utf-8")
        if not any(f"= {boss}," in line or f"= {boss}\n" in line for line in content.splitlines(keepends=True)):
            raise ValueError(f"Missing script boss identity: {script} {boss}")
        inputs[str(script.relative_to(root)).replace("\\", "/")] = hashlib.sha256(script.read_bytes()).hexdigest()
        normal = creatures.get(boss)
        for mode in range(len(MAPS[map_id][2])):
            entry = int(normal[f"difficulty_entry_{mode}"]) if mode else boss
            template = creatures.get(entry)
            if template and int(template["lootid"]):
                spawn = {"phaseMask": "1", "guid": "0", "_line": 0, "script_basis": str(script.relative_to(root))}
                visit(spawn, template, map_id, mode, "creature_loot_template", int(template["lootid"]), "creature")
    # Explicit script chest associations: Malygos gifts and the 50-attempt heroic ToC tribute chests.
    chests = [(616, 0, 193905, "Northrend/Nexus/EyeOfEternity/eye_of_eternity.h"),
              (616, 1, 193967, "Northrend/Nexus/EyeOfEternity/eye_of_eternity.h"),
              (649, 2, 195665, "Northrend/CrusadersColiseum/TrialOfTheCrusader/trial_of_the_crusader.h"),
              (649, 3, 195669, "Northrend/CrusadersColiseum/TrialOfTheCrusader/trial_of_the_crusader.h")]
    for map_id, mode, entry, header_name in chests:
        header = root / "src/server/scripts" / header_name
        inputs[str(header.relative_to(root)).replace("\\", "/")] = hashlib.sha256(header.read_bytes()).hexdigest()
        if str(entry) not in header.read_text(encoding="utf-8"):
            raise ValueError(f"Script chest identity changed: {entry}")
        template = objects[entry]
        spawn = {"phaseMask": "1", "guid": "0", "_line": 0, "script_basis": str(header.relative_to(root))}
        # Ceiling evidence only: acquisition can require a lock/event, so do not invent source proof.
        template = dict(template, Data0="0", Data6="0", Data8="0")
        visit(spawn, template, map_id, mode, "gameobject_loot_template", int(template["Data1"]), "gameobject")
    catalog = []
    for item, template in sorted(eligible.items()):
        catalog.append({"item": item, "ilvl": int(template["ItemLevel"]),
                        "required_level": int(template["RequiredLevel"]),
                        "inventory_type": int(template["InventoryType"]),
                        "class": int(template["class"]), "subclass": int(template["subclass"]),
                        "quality": int(template["Quality"]), "template_line": template["_line"],
                        "observed_source": observed.get(item), "verified_dungeon": dungeon.get(item)})
    return {"schema": 1, "scope": "Base SQL snapshot; not historical introduction or live eligibility proof",
            "era_guards": {"Classic_exclusive_item_id": 23728, "TBC_exclusive_item_id": 35570},
            "inputs_sha256": inputs, "ceilings": [caps[k] for k in sorted(caps)], "items": catalog}


def render(data):
    if len(data["items"]) > 65536:
        raise ValueError("Catalog no longer fits uint16 references; widen indices explicitly")
    lines = ["// Generated offline by tools/build_candidate_catalog.py. See data/equipment-candidate-catalog.json.",
             "CandidateCatalogEntry const CandidateCatalog[] =", "{"]
    for r in data["items"]:
        observed = r["observed_source"]["stage"] if r["observed_source"] else 0
        dungeon = r["verified_dungeon"]["stage"] if r["verified_dungeon"] else 0
        mode = r["observed_source"]["difficulty"] if r["observed_source"] else 0
        raid = r["observed_source"]["raid"] if r["observed_source"] else False
        lines.append(f"    {{{r['item']}, {r['ilvl']}, {r['required_level']}, {r['inventory_type']}, "
                     f"{r['class']}, {r['subclass']}, {r['quality']}, {observed}, {dungeon}, "
                     f"{mode}, {str(raid).lower()}}},")
    lines += ["};", "CandidateCeiling const CandidateCeilings[] =", "{"]
    for r in data["ceilings"]:
        lines.append(f"    {{{r['map']}, {r['difficulty']}, {r['capacity']}, {r['stage']}, {r['ilvl']}, {r['item']}}},")
    lines += ["};", "uint16 const CandidatePoolIndices[] =", "{"]
    offsets = [0]
    for slot in range(19):
        indices = [i for i, r in enumerate(data["items"]) if slot in SLOTS[r["inventory_type"]]]
        indices.sort(key=lambda i: (-data["items"][i]["ilvl"], data["items"][i]["item"]))
        for start in range(0, len(indices), 12):
            lines.append("    " + ", ".join(map(str, indices[start:start + 12])) + ",")
        offsets.append(offsets[-1] + len(indices))
    lines += ["};", "uint32 const CandidatePoolOffsets[] =", "{"]
    for start in range(0, len(offsets), 10):
        lines.append("    " + ", ".join(map(str, offsets[start:start + 10])) + ",")
    lines += ["};", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    module = Path(__file__).resolve().parents[1]
    data = generate(reference_root())
    outputs = {module / "data/equipment-candidate-catalog.json": json.dumps(data, ensure_ascii=False, indent=2) + "\n",
               module / "src/RaidEquipmentCandidateData.inc": render(data)}
    for path, content in outputs.items():
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != content:
                raise SystemExit(f"Stale candidate catalog: {path}")
        else:
            path.write_text(content, encoding="utf-8", newline="\n")
    print(f"Candidates {len(data['items'])}; map/difficulty ceilings {len(data['ceilings'])}; check={args.check}")
    print([(r["map"], r["difficulty"], r["ilvl"], r["item"]) for r in data["ceilings"]])


if __name__ == "__main__":
    main()
