"""Generate advisory short sets and exact BiS references. Never score a live bot.

Fallback hints use advisory SQL stat weights, not live spec-sensitive optimality.
Runtime consumes them without changing exhaustive selection or tier priority.
"""

import argparse
import hashlib
import json
from pathlib import Path
from source_context import input_name, reference_root
import re

from build_candidate_indexes import CLASSES, build
from build_source_manifest import sql_rows


KEYS = {249: 258, 309: 78, 409: 78, 469: 83, 509: 78, 531: 88, 532: 125, 533: 224,
        534: 156, 544: 125, 548: 141, 550: 141, 564: 156, 565: 125, 568: 156, 580: 164,
        603: 245, 615: 224, 616: 224, 631: 264, 649: 258, 724: 290}


def combinations():
    for cls in CLASSES:
        for tab in range(3):
            role = 2
            if cls == 1:
                role = 0 if tab == 2 else 3
            elif cls == 2:
                role = [1, 0, 3][tab]
            elif cls in [4, 6]:
                role = 3
            elif cls == 5:
                role = 2 if tab == 2 else 1
            elif cls == 7:
                role = [2, 3, 1][tab]
            elif cls == 11:
                role = [2, 3, 1][tab]
            yield cls, tab, role
        if cls == 6:
            yield cls, 0, 0
        if cls == 11:
            yield cls, 10, 0


def advisory_score(template, cls, tab, role):
    # Deliberately separate from StatsWeightCalculator. These weights only order hints;
    # no hint can stop the production exhaustive search or authorize an equip.
    if role == 0:
        weights = {7: 2, 12: 3, 13: 2, 14: 2, 15: 2, 3: 1, 4: 1}
    elif role == 1:
        weights = {45: 3, 43: 4, 5: 1, 6: 1 if cls != 5 or tab == 1 else 0.5}
    elif role == 2 and cls != 3:
        weights = {45: 3, 5: 1, 6: 0.5, 18: 1, 21: 1, 31: 1, 32: 1, 36: 1}
    else:
        weights = {3: 2 if cls in [3, 4, 7, 11] else 1, 4: 2 if cls in [1, 2, 6] else 1,
                   38: 1, 39: 1, 31: 1, 32: 1, 36: 1, 37: 1}
    value = sum(float(template[f"stat_value{i}"]) * weights.get(int(template[f"stat_type{i}"]), 0)
                for i in range(1, 11))
    if role == 0:
        value += int(template["armor"]) * 0.02 + int(template["block"]) * 0.2
    if role == 3 or cls == 3:
        delay = max(1, int(template["delay"]))
        value += sum(float(template[f"dmg_min{i}"]) + float(template[f"dmg_max{i}"])
                     for i in [1, 2]) * 2500 / delay
    return value


def role_hint_fits(item, cls, tab, role, slot):
    if role == 0 and item["class"] == 4 and slot != 14 and 1 <= item["subclass"] <= 4:
        if item["subclass"] != (2 if cls == 11 else 4):
            return False
    if role == 0 and cls in [1, 2] and slot == 16 and item["inventory_type"] != 14:
        return False
    if item["class"] != 2:
        return True
    if role == 0 and cls not in [6, 11] and item["inventory_type"] == 17:
        return False
    needs_two_hand = (cls == 1 and tab == 0 or cls == 2 and role == 3 or
                      cls == 11 and role in [0, 3] or cls == 6 and (role == 0 or tab in [0, 2]))
    return not (slot == 15 and needs_two_hand and item["inventory_type"] != 17)


def generate(module):
    root = reference_root()
    catalog_path = module / "data/equipment-candidate-catalog.json"
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    seed = json.loads((module / "data/equipment-class-proficiencies.json").read_text(encoding="utf-8"))
    views = build(root, catalog, seed)
    templates = {int(r["entry"]): r for r in sql_rows(root / "data/sql/base/db_world/item_template.sql")}
    items = catalog["items"]
    lookup = {x["item"]: i for i, x in enumerate(items)}
    source = root / "modules/mod-playerbots/data/sql/playerbots/updates/2026_04_28_00_playerbots_bis_gear.sql"
    text = source.read_text(encoding="utf-8")
    rows = [tuple(map(int, m.groups())) for m in re.finditer(
        r"^\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,", text, re.M)]
    if len(rows) != sum(line.lstrip().startswith("(") for line in text.splitlines()):
        raise ValueError("Unsupported BiS tuple layout")
    raw = {}
    for cls, tab, slot, faction, key, item in rows:
        raw.setdefault((key, cls, tab, faction), {})[slot] = item
    bis = []
    for key, cls, tab in sorted({r[:3] for r in raw}):
        for faction in [1, 2]:
            merged = dict(raw.get((key, cls, tab, 0), {}))
            merged.update(raw.get((key, cls, tab, faction), {}))
            valid = {slot: lookup[item] for slot, item in merged.items() if slot < 19 and item in lookup}
            if valid:
                bis.append(dict(key=key, cls=cls, tab=tab, faction=faction,
                                present=sum(1 << s for s in valid), indices=[valid.get(s, 0) for s in range(19)]))
    sets = []
    scores = {combo: [advisory_score(templates[x["item"]], *combo) for x in items]
              for combo in combinations()}
    for p in catalog["ceilings"]:
        for cls, tab, role in sorted(combinations()):
            for faction in [1, 2]:
                hints = []
                for slot in range(19):
                    bucket = ((p["stage"] - 1) * 10 + CLASSES.index(cls)) * 19 + slot
                    first, last = views["offsets"][bucket:bucket + 2]
                    eligible = []
                    for index in views["indices"][first:last]:
                        item = items[index]
                        flags = int(templates[item["item"]]["FlagsExtra"])
                        if item["ilvl"] > p["ilvl"] or (flags & 1 and faction == 1 or flags & 2 and faction == 2):
                            continue
                        level = 60 if p["stage"] < 7 else 70 if p["stage"] < 13 else 80
                        if item["required_level"] < level - 20 or not role_hint_fits(item, cls, tab, role, slot):
                            continue
                        observed = item["observed_source"]
                        modes = 5 if p["difficulty"] == 2 else (1 << (p["difficulty"] + 1)) - 1
                        if observed and observed["raid"] and observed["stage"] == p["stage"] and (
                                observed["difficulty"] >= 4 or not modes & (1 << observed["difficulty"])):
                            continue
                        eligible.append(index)
                    eligible.sort(key=lambda i: (-scores[(cls, tab, role)][i], -items[i]["ilvl"], items[i]["item"]))
                    hints.append(eligible[:4])
                sets.append(dict(map=p["map"], capacity=p["capacity"], difficulty=p["difficulty"],
                                 cls=cls, tab=tab, role=role, faction=faction, bis_key=KEYS[p["map"]], hints=hints))
    sets.sort(key=lambda x: tuple(x[k] for k in ["map", "difficulty", "capacity", "cls", "tab", "role", "faction"]))
    return {"schema": 1, "policy": "strict legacy result; advisory SQL stat hints only", "bis": bis, "sets": sets,
            "inputs": {input_name(p, root, module): hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in [source, catalog_path, root / "data/sql/base/db_world/item_template.sql"]}}


def render(data):
    lines = ["// Generated by tools/build_preparation_sets.py; no runtime scores are baked in.",
             "PreparedBisRecord const PreparedBisRecords[] =", "{"]
    for row in data["bis"]:
        lines += [f"    {{{row['key']}, {row['cls']}, {row['tab']}, {row['faction']}, {row['present']},",
                  "        {{" + ", ".join(map(str, row["indices"][:10])) + ",",
                  "          " + ", ".join(map(str, row["indices"][10:])) + "}}},"]
    lines += ["};", "PreparedSetRecord const PreparedSetRecords[] =", "{"]
    for row in data["sets"]:
        lines += ["    {" + ", ".join(str(row[k]) for k in
                  ["map", "capacity", "difficulty", "cls", "tab", "role", "faction", "bis_key"]) + ", {{"]
        for hint in row["hints"]:
            lines.append("        {{" + ", ".join(map(str, hint + [0] * (4 - len(hint)))) + "}},")
        lines += ["    }}, {{" + ", ".join(str(len(h)) for h in row["hints"]) + "}}},"]
    return "\n".join(lines + ["};", ""])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    module = Path(__file__).resolve().parents[1]
    data = generate(module)
    for path, value in [(module / "data/preparation-sets.json", json.dumps(data, indent=2) + "\n"),
                        (module / "src/RaidPreparationSetData.inc", render(data))]:
        if args.check:
            if path.read_text(encoding="utf-8") != value:
                raise SystemExit(f"Stale preparation sets: {path}")
        else:
            path.write_text(value, encoding="utf-8", newline="\n")
    print(f"Prepared sets: {len(data['sets'])}; merged BiS records: {len(data['bis'])}; check={args.check}")
