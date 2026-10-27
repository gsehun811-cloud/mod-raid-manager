# Equipment source validation

This describes the strict source-evidence foundation. The later policy in
[EQUIPMENT_FALLBACK.md](EQUIPMENT_FALLBACK.md) now consumes it alongside an ilvl-bounded
unverified fallback path. Source proof remains strict: permitting an unverified item
under the fallback policy never changes its proof status to verified. Composition,
AI, and the item-preserving `ApplyPreparedItem` execution path remain unchanged.

## Evidence and policy are separate

`SourceProof::SqlSnapshotPath` proves an acquisition association in the repository's
**base SQL snapshot**, not in a connected database. It follows:

1. An explicit creature or chest spawn on a known map, with a matching spawn mask.
2. The creature template for that exact difficulty, or the chest's `Data1` loot ID.
3. Creature/chest loot and any nested reference loot leading to the item.

Evidence retains input SHA-256 hashes, spawn GUID/masks, table names, template and
loot row line numbers, reference/group IDs, and loot modes. IDs in runtime logs
resolve to the matching `evidence` field in `data/equipment-source-evidence.json`.
One representative path per item/map/mode/kind/proof is retained, not every spawn
or every acquisition method. IDs are regenerated with the manifest; keep the JSON
with the corresponding binary when interpreting a log.

Map unlock stages are **explicit curated policy**, not facts parsed from a BiS key,
item level, item ID range, or a `Phase` label. A snapshot path is distinguished from
that policy and from actual character eligibility. Earlier stages are ordered as:

- Classic dungeons, MC/Onyxia era, BWL, ZG, AQ, Naxx.
- TBC dungeons, T4, T5, T6, ZA, Sunwell.
- Wrath dungeons, T7, Ulduar, ToC, ICC, Ruby Sanctum.

Dire Maul, Magisters' Terrace, Trial of the Champion, and the ICC five-player
dungeons have separate explicit gates. AQ20 is gated at AQ despite its current
BiS key of 78; ZG is gated after BWL despite also using 78. The ordering represents
the default release progression, not every server's individual unlock state.

`GetEquipmentSourcePolicy` checks actual map/difficulty/capacity. Changing the
profile's BiS key never grants a source tier. Ten-player heroic does not authorize
25-player normal sources. Wrath SQL for maps 249/533 is not treated as evidence for
custom Classic 40-player versions. VoA and unknown/custom maps have no default policy.

`SourcePolicy` can accept an explicitly authorized stage ceiling and separate raid
and dungeon difficulty masks in a future candidate selector or difficulty feature.
It does not implement `.raiddiff`, character progression, or authorize tier changes.

## Fail-closed limits

The manifest covers the unique item IDs referenced by the repository BiS SQL.
Items without a supported path are listed explicitly in `unverified_items` and
`unverified_details`; this does not prove that they have no acquisition method.

The following are not promoted to verified proof:

- Unknown maps, absent/script-created spawns, absent difficulty templates, cycles,
  and missing reference tables. Difficulty fallback is deliberately not guessed.
- Conditional or quest-only loot, non-default loot modes, event/phase-dependent
  spawns, holiday items, saturated chance groups, and locked/event/quest chests.
- Vendors/currencies, quests/rewards, crafting, PvP, item containers, and other
  scripted acquisition paths. These need their own evidence adapters later.

Grouped reference handling follows this repository's `LootTemplate::AddEntry` and
`LootGroup::Process`: the parent's reference group number does **not** select the
same-numbered child group. Tests protect this distinction and cyclic references.

Merged SQL updates, runtime overrides, DBC/script/event eligibility, historical
loot revisions, earliest item introduction dates, and later drop removals are not
proved by this snapshot. The map gate is a conservative policy model, not historical
loot verification. An `Allowed` result means only that a supported snapshot source
passes that map-stage/difficulty policy. It is not a full equipment readiness or
acquisition-success result. Automatic selection must additionally validate
live item requirements, progression, role, improvement, source freshness, and any
required historical/custom policy. It must preserve existing gear on uncertainty.

## APIs and diagnostics

- `GetEquipmentSources(item)`: all representative verified/unverified source records.
- `GetEquipmentSourcePolicy(profile)`: map policy independent of BiS keys.
- `AssessEquipmentSource(item, policy)`: unverified, unknown policy, later stage,
  blocked difficulty, or allowed source. Multiple paths succeed if one is allowed.
- `DiagnoseEquipmentCandidate`: separately checks missing runtime templates,
  wrong inventory slot, and actual bot faction/race incompatibility. It creates
  no items and calls no mutation or DB API.
- `SourceDiagnostics::EligibleForAutomaticSelection`: strict verified-source path.
  The separate candidate selector may admit an unverified source after additional
  ilvl/era/role/core checks; this does not change this strict API's result.

During the existing staged `raidprep` gear work, an INFO entry in
`module.raidmanager` records source status, evidence ID, and structural flags.
The source diagnostic itself does not mutate items. The candidate selector chooses
the plan separately, and the existing `ApplyPreparedItem` path executes it.

The offline manifest also diagnoses raw SQL wrong-slot rows and effective faction
conflicts after shared rows are merged with faction overrides. These are separate
from source proof. Two-handed offhands are structurally possible and still need
the core's actual talent/equip check.

## Reproduction and verification

Run from the module root with a compatible reference checkout configured as described
in DEVELOPMENT.md. No database, third-party Python package or server is needed:

```text
python tools/build_source_manifest.py
python tools/build_source_manifest.py --check
python tools/test_source_manifest.py
```

The generator reads only explicitly named source SQL files. It writes only the
module's evidence JSON and generated C++ include. Unsupported SQL dump layouts
raise an error instead of silently dropping data. `--check` performs no writes.
Regenerate the include and JSON together after source SQL or map-policy changes.

The initial dataset contains 6,647 BiS rows and 2,037 unique item IDs. Of these,
543 have a supported SQL snapshot path and 1,494 remain unverified. There are
1,121 representative source records: 796 proved paths and 325 unresolved paths.
An item may have several records, so path counts and unique item counts differ.
The manifest hashes 12 input SQL files. It is authoritative after regeneration.
Structural diagnostics reproduce five wrong-slot rows and 114 effective faction conflicts.
For example, item 51842 has an ICC heroic source, even though a BiS row uses it at
key 245; it must not be approved by an Ulduar source policy. Item 50719 in druid
slot 17 is diagnosed as a holdable in the wrong slot.

Use the core build workflow in README.md and DEVELOPMENT.md. Generators do not
connect to a database; normal runtime persistence is described in COMPATIBILITY.md.
