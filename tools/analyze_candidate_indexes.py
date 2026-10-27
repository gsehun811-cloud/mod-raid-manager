"""Offline pool-search model: both pools exhausted, no 3ms time stops or warm-up.

Uniformly weights ten classes and nineteen slots. This estimates work turns, not
actual raid preparation duration, live scoring, or N150 CPU utilization.
"""

import json
from pathlib import Path
from source_context import reference_root

from build_candidate_catalog import SLOTS
from build_candidate_indexes import CLASSES, build, stage_allowed


def admissible(item, profile):
    source = item["observed_source"]
    stage = profile["stage"]
    level = 60 if stage < 7 else 70 if stage < 13 else 80
    difficulty = profile["difficulty"]
    modes = 5 if difficulty == 2 else (1 << (difficulty + 1)) - 1
    return (stage_allowed(item, stage) and item["ilvl"] <= profile["ilvl"] and
            item["required_level"] >= level - 20 and not (
                source and source["raid"] and source["stage"] == stage and
                (source["difficulty"] >= 4 or not modes & (1 << source["difficulty"]))))


def scan(sequence, items, profile, dungeon):
    turns = evaluations = cursor = 0
    while cursor < len(sequence):
        checked = evaluated = 0
        while cursor < len(sequence) and checked < 256 and evaluated < 8:
            item = items[sequence[cursor]]
            cursor += 1
            checked += 1
            if admissible(item, profile) and (not dungeon or item["verified_dungeon"] and
                                             item["verified_dungeon"]["stage"] <= profile["stage"]):
                evaluated += 1
        turns += 1
        evaluations += evaluated
    return max(1, turns), evaluations


def main():
    module = Path(__file__).resolve().parents[1]
    catalog = json.loads((module / "data/equipment-candidate-catalog.json").read_text(encoding="utf-8"))
    seed = json.loads((module / "data/equipment-class-proficiencies.json").read_text(encoding="utf-8"))
    index = build(reference_root(), catalog, seed)
    items = catalog["items"]
    pools = []
    for slot in range(19):
        pool = [i for i, item in enumerate(items) if slot in SLOTS[item["inventory_type"]]]
        pools.append(sorted(pool, key=lambda i: (-items[i]["ilvl"], items[i]["item"])))
    results = []
    for map_id, difficulty, capacity in [(309, 0, 20), (532, 0, 10), (631, 0, 10), (724, 3, 25)]:
        profile = next(p for p in catalog["ceilings"] if
                       (p["map"], p["difficulty"], p["capacity"]) == (map_id, difficulty, capacity))
        totals = [0] * 6
        for cls_index, cls in enumerate(CLASSES):
            for slot in range(19):
                old = [i for i in pools[slot] if items[i]["ilvl"] <= profile["ilvl"]]
                bucket = ((profile["stage"] - 1) * 10 + cls_index) * 19 + slot
                first, last = index["offsets"][bucket:bucket + 2]
                new = [i for i in index["indices"][first:last] if items[i]["ilvl"] <= profile["ilvl"]]
                totals[0] += len(old)
                totals[1] += len(new)
                for sequence, offset in [(old, 2), (new, 4)]:
                    for dungeon in [True, False]:
                        turns, evaluations = scan(sequence, items, profile, dungeon)
                        totals[offset] += turns
                        totals[offset + 1] += evaluations
        names = ["slot_candidates_old", "slot_candidates_new", "two_pool_turns_old",
                 "detailed_calls_old", "two_pool_turns_new", "detailed_calls_new"]
        results.append(dict(map=map_id, difficulty=difficulty, capacity=capacity,
                            **{name: round(value / 190, 2) for name, value in zip(names, totals)}))
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
