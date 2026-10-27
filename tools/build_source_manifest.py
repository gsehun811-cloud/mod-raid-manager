"""Build an offline, fail-closed SQL snapshot manifest. Never connect to MySQL.

Only explicit spawns, explicit difficulty templates, and creature/chest loot paths
are supported. Policy stages are curated map policy, not facts inferred from BiS.
"""

import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
from source_context import reference_root
import re


# map: (stage, raid, difficulty -> actual capacity). No item-level/BiS-key inference.
MAPS = {
    309: ("ClassicZG", True, [20]),
    409: ("ClassicMC", True, [40]),
    469: ("ClassicBWL", True, [40]),
    509: ("ClassicAQ", True, [20]),
    531: ("ClassicAQ", True, [40]),
    532: ("TbcT4", True, [10]),
    534: ("TbcT6", True, [25]),
    544: ("TbcT4", True, [25]),
    548: ("TbcT5", True, [25]),
    550: ("TbcT5", True, [25]),
    564: ("TbcT6", True, [25]),
    565: ("TbcT4", True, [25]),
    568: ("TbcZA", True, [10]),
    580: ("TbcSunwell", True, [25]),
    249: ("WrathToc", True, [10, 25]),
    533: ("WrathT7", True, [10, 25]),
    615: ("WrathT7", True, [10, 25]),
    616: ("WrathT7", True, [10, 25]),
    603: ("WrathUlduar", True, [10, 25]),
    649: ("WrathToc", True, [10, 25, 10, 25]),
    631: ("WrathIcc", True, [10, 25, 10, 25]),
    724: ("WrathRuby", True, [10, 25, 10, 25]),
}
for map_id in [33, 34, 36, 43, 47, 48, 70, 90, 109, 129, 189, 209, 229, 230, 289, 329, 349]:
    MAPS[map_id] = ("ClassicDungeons", False, [5])
# Dire Maul is deliberately not classified as launch-era dungeons.
MAPS[429] = ("ClassicMC", False, [5])
for map_id in [269, 540, 542, 543, 545, 546, 547, 552, 553, 554, 555, 556, 557, 558, 560]:
    MAPS[map_id] = ("TbcDungeons", False, [5, 5])
MAPS[585] = ("TbcSunwell", False, [5, 5])
for map_id in [574, 575, 576, 578, 595, 599, 600, 601, 602, 604, 608, 619]:
    MAPS[map_id] = ("WrathDungeons", False, [5, 5])
MAPS[650] = ("WrathToc", False, [5, 5])
for map_id in [632, 658, 668]:
    MAPS[map_id] = ("WrathIcc", False, [5, 5])


def sql_rows(path):
    """Read the repository's one-tuple-per-line dump format, with SQL escaping.

    Reject unsupported layout instead of silently producing incomplete evidence.
    Return named columns and source lines; no SQL is executed.
    """
    columns = []
    rows = []
    in_values = False
    with path.open(encoding="utf-8") as source:
        for number, line in enumerate(source, 1):
            match = re.match(r"^  `([^`]+)`", line)
            if match:
                columns.append(match[1])
            if line.startswith("INSERT INTO "):
                if not line.rstrip().endswith("VALUES"):
                    raise ValueError(f"Unsupported INSERT layout: {path}:{number}")
                in_values = True
                continue
            if not in_values:
                continue
            text = line.strip()
            if not text:
                continue
            if not text.startswith("(") or not text.endswith(("),", ");")):
                raise ValueError(f"Unsupported tuple layout: {path}:{number}")
            values = next(csv.reader([text[:-1][1:-1]], quotechar="'", escapechar="\\"))
            if len(values) != len(columns):
                raise ValueError(f"Column mismatch: {path}:{number}")
            row = dict(zip(columns, values))
            row["_line"] = number
            rows.append(row)
            if text.endswith(";"):
                in_values = False
    if in_values or not columns or not rows:
        raise ValueError(f"Missing/incomplete SQL data: {path}")
    return rows


def loot_paths(stores, conditions, table, entry, selected_group=0, ancestors=()):
    """Follow this core's grouped-reference processing; retain unresolved conditions.

    A path proves a snapshot drop association, not quest/event/script eligibility.
    """
    key = (table, entry, selected_group)
    if key in ancestors:
        return
    for row in stores[table].get(entry, []):
        ref = abs(int(row["Reference"]))
        group = int(row["GroupId"])
        if selected_group and group != selected_group:
            continue
        reasons = []
        if (table, entry) in conditions:
            reasons.append("loot_conditions_not_evaluated")
        if int(row["QuestRequired"]):
            reasons.append("quest_required")
        if not int(row["LootMode"]) & 1:
            reasons.append("nondefault_loot_mode")
        if int(row["MinCount"]) < 1 or int(row["MaxCount"]) < 1:
            reasons.append("invalid_count")
        if float(row["Chance"]) < 0 or (float(row["Chance"]) == 0 and not group):
            reasons.append("unsupported_chance")
        if group:
            total = sum(max(0, float(peer["Chance"])) for peer in stores[table][entry]
                        if int(peer["GroupId"]) == group and int(peer["LootMode"]) & 1)
            if total > 100 or (total >= 100 and float(row["Chance"]) == 0):
                reasons.append("saturated_loot_group")
        step = {"table": table, "entry": entry, "line": row["_line"], "item": int(row["Item"]),
                "reference": ref, "group": group, "loot_mode": int(row["LootMode"])}
        if ref:
            # LootTemplate::AddEntry puts grouped references into LootGroup. Its Process
            # calls the referenced template with groupId=0, not the parent's group number.
            for item, chain, child_reasons in loot_paths(
                    stores, conditions, "reference_loot_template", ref, 0, ancestors + (key,)):
                yield item, [step] + chain, reasons + child_reasons
        else:
            yield int(row["Item"]), [step], reasons


def diagnose_bis(rows, templates):
    inventory_types = {
        0: {1}, 1: {2}, 2: {3}, 4: {5, 20}, 5: {6}, 6: {7}, 7: {8}, 8: {9}, 9: {10},
        10: {11}, 11: {11}, 12: {12}, 13: {12}, 14: {16}, 15: {13, 17, 21},
        16: {13, 14, 17, 22, 23}, 17: {15, 25, 26, 28},
    }
    wrong_slots = []
    missing_templates = []
    lookup = {}
    for cls, tab, slot, faction, key, item in rows:
        template = templates.get(item)
        if not template:
            missing_templates.append([key, cls, tab, faction, slot, item])
        elif int(template["InventoryType"]) not in inventory_types.get(slot, set()):
            wrong_slots.append([key, cls, tab, faction, slot, item])
        lookup[key, cls, tab, faction, slot] = item
    conflicts = []
    # Standard WotLK playable race masks, not a claim about any specific bot's race.
    races = {1: [1, 3, 4, 7, 11], 2: [2, 5, 6, 8, 10]}
    for key, cls, tab, slot in sorted({(k, c, t, s) for k, c, t, f, s in lookup}):
        for faction in [1, 2]:
            item = lookup.get((key, cls, tab, faction, slot), lookup.get((key, cls, tab, 0, slot)))
            template = templates.get(item)
            if not template:
                continue
            flags = int(template["FlagsExtra"])
            race_mask = int(template["AllowableRace"])
            team_mask = sum(1 << (race - 1) for race in races[faction])
            if ((faction == 1 and flags & 1) or (faction == 2 and flags & 2) or
                    not race_mask & team_mask):
                conflicts.append([key, cls, tab, faction, slot, item])
    return {"tuple_columns": ["bis_key", "class", "tab", "faction", "slot", "item"],
            "wrong_slot_rows": wrong_slots, "missing_template_rows": missing_templates,
            "effective_faction_conflicts": conflicts}


def build(root):
    base = root / "data/sql/base/db_world"
    files = {}

    def read(table):
        path = base / f"{table}.sql"
        files[str(path.relative_to(root)).replace("\\", "/")] = hashlib.sha256(path.read_bytes()).hexdigest()
        return sql_rows(path)

    bis_path = root / "modules/mod-playerbots/data/sql/playerbots/updates/2026_04_28_00_playerbots_bis_gear.sql"
    bis_text = bis_path.read_text(encoding="utf-8")
    bis_rows = [tuple(map(int, match.groups())) for match in re.finditer(
        r"^\((\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),", bis_text, re.M)]
    if len(bis_rows) != len(re.findall(r"^\(", bis_text, re.M)):
        raise ValueError("Unsupported BiS tuple layout")
    files[str(bis_path.relative_to(root)).replace("\\", "/")] = hashlib.sha256(bis_path.read_bytes()).hexdigest()
    wanted = {row[5] for row in bis_rows}
    item_templates = {int(row["entry"]): row for row in read("item_template")}
    diagnostics = diagnose_bis(bis_rows, item_templates)
    stores = {}
    for table in ["creature_loot_template", "gameobject_loot_template", "reference_loot_template"]:
        store = collections.defaultdict(list)
        for row in read(table):
            store[int(row["Entry"])].append(row)
        stores[table] = store
    condition_types = {1: "creature_loot_template", 4: "gameobject_loot_template", 10: "reference_loot_template"}
    conditions = set()
    for row in read("conditions"):
        table = condition_types.get(int(row["SourceTypeOrReferenceId"]))
        if table:
            conditions.add((table, int(row["SourceGroup"])))
    templates = {int(row["entry"]): row for row in read("creature_template")}
    objects = {int(row["entry"]): row for row in read("gameobject_template")}
    event_spawns = {
        "Creature": {int(row["guid"]) for row in read("game_event_creature")},
        "Chest": {int(row["guid"]) for row in read("game_event_gameobject")},
    }
    candidates = {}
    seen_roots = set()

    def add(spawn, template, map_id, difficulty, table, loot, kind, template_entry):
        event_bound = int(spawn["guid"]) in event_spawns[kind]
        chest_restricted = kind == "Chest" and any(int(template[f"Data{i}"]) for i in [0, 6, 8])
        key = (map_id, difficulty, table, loot, int(spawn["phaseMask"]), event_bound, chest_restricted)
        if key in seen_roots:
            return
        seen_roots.add(key)
        for item, chain, reasons in loot_paths(stores, conditions, table, loot):
            if item not in wanted:
                continue
            reasons = set(reasons)
            if int(spawn["phaseMask"]) != 1:
                reasons.add("nondefault_spawn_phase")
            if event_bound:
                reasons.add("event_bound_spawn")
            if chest_restricted:
                reasons.add("chest_lock_event_or_quest")
            item_template = item_templates.get(item)
            if not item_template:
                reasons.add("missing_item_template")
            elif int(item_template["HolidayId"]):
                reasons.add("holiday_item")
            record = {"item": item, "map": map_id, "difficulty": difficulty, "kind": kind,
                      "stage": MAPS[map_id][0], "raid": MAPS[map_id][1],
                      "proof": "Unverified" if reasons else "SqlSnapshotPath", "reasons": sorted(reasons),
                      "spawn_table": "creature" if kind == "Creature" else "gameobject",
                      "spawn_line": spawn["_line"], "spawn_guid": int(spawn["guid"]),
                      "spawn_mask": int(spawn["spawnMask"]), "phase_mask": int(spawn["phaseMask"]),
                      "template_entry": template_entry, "template_line": template["_line"], "loot_path": chain}
            # One representative path per item/map/mode/kind/proof, not a complete acquisition catalogue.
            key = (item, map_id, difficulty, kind, record["proof"])
            if key not in candidates:
                candidates[key] = record

    for spawn in read("creature"):
        map_id = int(spawn["map"])
        if map_id not in MAPS:
            continue
        for field in ["id1", "id2", "id3"]:
            normal = templates.get(int(spawn[field]))
            if not normal:
                continue
            for difficulty in range(len(MAPS[map_id][2])):
                if not int(spawn["spawnMask"]) & (1 << difficulty):
                    continue
                entry = int(normal[f"difficulty_entry_{difficulty}"]) if difficulty else int(normal["entry"])
                # Do not guess heroic/difficulty fallback or script-created creature presence.
                template = templates.get(entry)
                if template and int(template["lootid"]):
                    add(spawn, template, map_id, difficulty, "creature_loot_template",
                        int(template["lootid"]), "Creature", entry)
    for spawn in read("gameobject"):
        map_id = int(spawn["map"])
        template = objects.get(int(spawn["id"]))
        if map_id not in MAPS or not template or int(template["type"]) != 3:
            continue
        for difficulty in range(len(MAPS[map_id][2])):
            if int(spawn["spawnMask"]) & (1 << difficulty) and int(template["Data1"]):
                add(spawn, template, map_id, difficulty, "gameobject_loot_template",
                    int(template["Data1"]), "Chest", int(template["entry"]))
    records = [candidates[key] for key in sorted(candidates)]
    for evidence, record in enumerate(records, 1):
        record["evidence"] = evidence
    verified = {r["item"] for r in records if r["proof"] == "SqlSnapshotPath"}
    unverified = sorted(wanted - verified)
    unverified_details = []
    for item in unverified:
        reasons = {reason for r in records if r["item"] == item for reason in r["reasons"]}
        unverified_details.append({"item": item, "reasons": sorted(reasons) or ["no_supported_snapshot_path"]})
    return {"schema": 1, "scope": "Repository base SQL snapshot, not replayed updates or live DB",
            "policy_basis": "Explicit curated map release-stage policy; never derived from BiS keys or item levels",
            "limits": ["Snapshot drop association is not live acquisition or character eligibility",
                       "Unknown maps, dynamic spawns, missing difficulty templates and reference cycles fail closed",
                       "Vendors, currencies, crafting, quests, PvP, item containers and scripted paths not proved",
                       "Loot conditions, quests, events, special loot modes and chest restrictions remain unverified",
                       "Source stage does not prove earliest item introduction, "
                       "historical loot revisions or drop removals",
                       "Source maps 249/533 describe Wrath, not custom Classic 40-player variants"],
            "inputs_sha256": files, "bis_rows": len(bis_rows), "bis_unique_items": len(wanted),
            "verified_unique_items": len(verified), "unverified_unique_items": len(unverified),
            "unverified_items": unverified, "unverified_details": unverified_details,
            "bis_diagnostics": diagnostics, "records": records}


def render_include(manifest):
    lines = ["// Generated by tools/build_source_manifest.py; evidence and limitations are in data/.",
             "struct SourceMap", "{", "    uint32 map;", "    UnlockStage stage;", "    bool raid;",
             "    uint8 difficulties;", "    uint32 capacities[4];", "};", "SourceMap const SourceMaps[] =", "{"]
    for map_id, (stage, raid, caps) in sorted(MAPS.items()):
        padded = caps + [0] * (4 - len(caps))
        lines.append(f"    {{{map_id}, UnlockStage::{stage}, {str(raid).lower()}, {(1 << len(caps)) - 1}, "
                     "{" + ", ".join(map(str, padded)) + "}},")
    lines += ["};", "EquipmentSource const SourceRecords[] =", "{"]
    for r in manifest["records"]:
        lines.append(f"    {{{r['item']}, {r['map']}, {r['difficulty']}, UnlockStage::{r['stage']}, "
                     f"SourceKind::{r['kind']},")
        lines.append(f"        SourceProof::{r['proof']}, {str(r['raid']).lower()}, {r['evidence']}}},")
    return "\n".join(lines + ["};", ""])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Read-only reproducibility check")
    args = parser.parse_args()
    module = Path(__file__).resolve().parents[1]
    manifest = build(reference_root())
    outputs = {
        module / "data/equipment-source-evidence.json": json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        module / "src/RaidEquipmentSourcesData.inc": render_include(manifest),
    }
    for path, content in outputs.items():
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != content:
                raise SystemExit(f"Stale manifest: {path}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
    print(json.dumps({key: manifest[key] for key in [
        "bis_rows", "bis_unique_items", "verified_unique_items", "unverified_unique_items"]}))
    print(f"Representative source records: {len(manifest['records'])}; check={args.check}")


if __name__ == "__main__":
    main()
