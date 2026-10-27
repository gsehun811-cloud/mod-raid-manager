"""Verify the published source/data bundle without a core checkout, compiler, DB or server."""

import hashlib
import json
from pathlib import Path
import re

from build_candidate_catalog import render as render_catalog
from build_preparation_sets import render as render_sets
from build_source_manifest import render_include as render_sources


def array(text, name):
    match = re.search(r"\b" + name + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not match:
        raise AssertionError(f"Missing generated array: {name}")
    return list(map(int, re.findall(r"\b\d+\b", match[1])))


def main():
    module = Path(__file__).resolve().parents[1]
    catalog = json.loads((module / "data/equipment-candidate-catalog.json").read_text())
    sources = json.loads((module / "data/equipment-source-evidence.json").read_text())
    sets = json.loads((module / "data/preparation-sets.json").read_text())
    summary = json.loads((module / "data/equipment-candidate-indexes.json").read_text())
    items = catalog["items"]
    assert len(items) == len({x["item"] for x in items}) <= 65536
    assert len(items) == summary["catalog_count"] == 23122
    for name, rendered in [("RaidEquipmentCandidateData.inc", render_catalog(catalog)),
                           ("RaidEquipmentSourcesData.inc", render_sources(sources)),
                           ("RaidPreparationSetData.inc", render_sets(sets))]:
        assert (module / "src" / name).read_text() == rendered, f"JSON/include mismatch: {name}"
    text = (module / "src/RaidEquipmentCandidateIndexes.inc").read_text()
    indices = array(text, "StageClassPoolIndices")
    offsets = array(text, "StageClassPoolOffsets")
    assert len(indices) == summary["reference_count"] == 1509139
    assert len(offsets) == 3421 and offsets[0] == 0 and offsets[-1] == len(indices)
    assert offsets == sorted(offsets) and all(0 <= i < len(items) for i in indices)
    for first, last in zip(offsets, offsets[1:]):
        order = [(-items[i]["ilvl"], items[i]["item"]) for i in indices[first:last]]
        assert order == sorted(order)
    assert len(array(text, "CandidateSnapshotClassMasks")) == len(items)
    assert len(array(text, "CandidateSnapshotUnsupported")) == len(items)
    assert len(catalog["ceilings"]) == 36 and len(sets["sets"]) == 2304 and len(sets["bis"]) == 818
    for record in sets["sets"]:
        assert len(record["hints"]) == 19
        assert all(len(hints) <= 4 and all(0 <= i < len(items) for i in hints) for hints in record["hints"])
    for record in sets["bis"]:
        assert len(record["indices"]) == 19
        assert all(0 <= i < len(items) for i in record["indices"])
    for path in (module / "src").glob("*"):
        for include in re.findall(r'#include "([^"]+)"', path.read_text()):
            if include.startswith("Raid") or include.endswith(".inc"):
                assert (module / "src" / include).is_file(), f"Missing local include: {include}"
    manifest = json.loads((module / "data/release-manifest.json").read_text())
    for relative, digest in manifest["files_sha256"].items():
        path = (module / relative).resolve()
        assert path.is_relative_to(module), "Unsafe manifest path"
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, relative
    print(f"Bundle OK: {len(manifest['files_sha256'])} file fingerprints; {len(items)} metadata records; "
          f"{len(indices)} uint16 references; 2304 sets; 818 merged BiS records; all local includes present")


if __name__ == "__main__":
    main()
