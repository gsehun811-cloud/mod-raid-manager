# Bounded equipment fallback

The strict-set/budgeted preparation revision is documented in
[PREPARATION_PERFORMANCE.md](PREPARATION_PERFORMANCE.md). It supersedes the common
200ms preparation pacing used in earlier development, while preserving equipment decisions.

This policy permits unverified acquisition sources under explicit ilvl, era, role,
and core-equip constraints. It supersedes the phase-1 observation-only integration.
It does not change recruitment, composition targets, talents, combat AI, boss AI,
Playerbots configuration/SQL, or the item-preserving equipment execution helper.

## Search order and incremental execution

For each required slot, preparation searches:

1. The exact current raid/class/tab/faction BiS item.
2. The same class/tab/faction in previous raid stages, nearest release stage first.
3. Verified dungeon candidates from the generated catalog.
4. The remaining bounded catalog, including items with unverified acquisition.
5. Retain existing equipment if no safe improvement is found.

A valid already-equipped exact BiS item is retained. If a valid exact candidate
does not improve current equipment, current equipment is retained. Lower-tier
search is explicit release-stage order, not subtraction from a BiS key. Shared
faction merging and the druid bear sentinel tab remain unchanged. A completely
missing exact specialization can still search previous tiers and both pools.

Search state stores IDs/cursors, never Player/AI pointers. Each resumable search microstep
examines at most 256 catalog entries and evaluates at most eight potential items,
with a soft 3ms search budget shared with the preparation slice. A slot can span
multiple slices; heavy equipment application is paced separately. Queries use the in-memory BiS cache; no new
runtime SQL query or server/database operation is introduced.

## Caps come from item data, not BiS labels

`tools/build_candidate_catalog.py` reads repository item templates and explicit
spawn/template/reference-loot paths. It derives a cap for each supported actual
map/difficulty/capacity from the highest eligible quality-2/3/4 item observed there.
Conditional boss drops can establish an upper bound without proving that a player
can acquire them. Special hard-mode loot modes, invalid counts/chances, saturated
groups, later-era required levels, and conservative era guards are excluded from
cap anchors. Script-declared bosses/chests supplement absent static spawns for
**ceiling and known-stage evidence only**, not verified acquisition proof.

Each cap records an actual anchor item, template ilvl, loot chain, SQL line numbers,
and any unresolved conditions/script association. Input hashes are retained in
`data/equipment-candidate-catalog.json`. At runtime the anchor must exist and its
ilvl must match the snapshot; otherwise equipment is retained. Existing items
above a cap are never confiscated or automatically downgraded.

Examples from this snapshot:

| Map/profile | Cap | BiS key |
| --- | ---: | ---: |
| ZG | 71 | 78 |
| MC | 78 | 78 |
| BWL | 83 | 83 |
| AQ20 | 74 | 78 |
| AQ40 | 88 | 88 |
| T4 | 125 | 125 |
| T5 | 141 | 141 |
| T6 | 151 | 156 |
| ZA | 141 | 156 |
| Sunwell | 164 | 164 |
| ToC 10 normal / 25 normal / 10 heroic / 25 heroic | 232 / 245 / 258 / 272 | 258 |
| ICC 10 normal / 25 normal / 10 heroic / 25 heroic | 264 / 271 / 271 / 284 | 264 |
| Ruby 10 normal / 25 normal / 10 heroic / 25 heroic | 258 / 271 / 271 / 284 | 290 |

ICC 10 normal's cap includes the actual 264 BoE trash drops in its SQL loot
association; it is not a claim that all its bosses drop 264 items. ToC heroic
caps include the script-declared 50-attempt tribute chests as upper-bound evidence;
that does not verify acquisition eligibility or grant the required conditions.
The generated manifest, not this example table, is authoritative after changes.
VoA, unknown custom variants, and Classic 40-player Onyxia/Naxx have no default
snapshot-backed cap. Such contexts preserve gear rather than use the numeric key.
An explicit BiS-key override does not authorize a larger cap or later stage.

## Unverified does not mean unrestricted

Every candidate must satisfy its cap, actual character level and era level cap
(60/70/80), structural slot/faction/race checks, Playerbots quality limits and
`IsValidItem`, and core `CanUseItem`/`CanEquipNewItem`. Required skills, spells,
reputation, unique-equipped and weapon/talent rules remain enforced. Missing
runtime metadata or changed snapshot identity is rejected.

Observed source stages are also indexed for the broader catalog, including
conditional paths. A clearly later known stage is rejected despite low ilvl;
a known higher raid difficulty at the same stage is also rejected. The strict
source API can reject additional known higher-stage/difficulty combinations.

Classic excludes candidate IDs >=23728 and TBC excludes IDs >=35570, including
low-required-level items. These are deliberately conservative legacy exclusion
guards, not proof of an item's expansion or acquisition. They can exclude valid
late-added old-expansion items. Low-ID backports/historical revisions cannot all
be identified from this SQL snapshot. Unknown-source approval remains an ilvl-
bounded heuristic and is logged as such, never as verified provenance.

The static catalog contains 23,122 template candidates and 36 cap profiles before
runtime restrictions. Pool search also excludes items requiring more than twenty
levels below the bot. Newly generated random-property/suffix candidates are not
supported because their actual result cannot be promised before creation.

## Role, improvement, and preservation

The evaluator must agree with the requested current role; it never changes AI
strategies to make a score fit. `StatsWeightCalculator` supplies spec-sensitive
weights, with quality/ilvl blending and speculative new set bonuses disabled.
Empty socket assumptions are removed and actual old enchants/gems are counted.
An improvement requires a finite positive score exceeding the old score by more
than 5% plus a small epsilon. Scores are a heuristic, not encounter readiness proof.

Additional non-regression floors protect tank stamina/armor/defense/avoidance/block,
healer healing/mana regeneration/intellect/spirit, caster spell power/hit, physical
hit/expertise, and relevant weapon DPS. Role-specific weapon constraints include
tank shields, Arms/retribution/feral mainhands and assassination daggers. Core
checks remain authoritative. Replacing random-property current gear is deferred;
replacing an existing set piece with a different set is deferred. These conservative
rules intentionally leave some theoretical upgrades unapplied.

Coupled weapon replacement that would displace an unscored offhand is deferred.
Candidate requirements, role, cap freshness and improvement are rechecked just
before `ApplyPreparedItem`. Bag space failure stops that slot without generating
further fallback items. Old equipment and bag contents are never cleared/deleted.
The original safe swap/storage path is retained, including bag-item reuse and
leaving a staged item in bags if an equip operation fails.

`module.raidmanager` logs search route, `sourceVerified`, cap, cap anchor, score,
and reason. Exact/previous BiS with unverified sources remain explicitly unverified
and are admitted only by the bounded checks. Retained equipment is not relabeled
as a newly verified candidate. Missing exact data remains a partial BiS diagnosis
even when a fallback equips successfully. Final verification uses the actual Item
overload of the core usability check.

## Offline checks

```text
python tools/build_candidate_catalog.py --check
python tools/test_candidate_catalog.py
python tools/test_source_manifest.py
powershell -File tools/run_candidate_tests.ps1
```

The native test compiles the actual production rules/catalog and performs 39
checks. Its entry point uses `.cpp.test` so it cannot be collected into worldserver.
MSVC warnings are errors in that test, with one compiler process. The tests do not
start a server or connect to a database. Runtime profession/inventory/AI behavior
is left to the existing core checks; no live-stack test or deployment is performed.

## Stage/class index and search diagnostics

`RaidEquipmentCandidateIndexes.inc` adds 18 progress stages x 10 playable classes
x 19 equipment slots. Each bucket is ordered by descending ilvl, then item ID,
matching the legacy pool. A binary search applies the unchanged profile ceiling.
The 23,122 metadata records remain in one catalog. Both the broad legacy pool
and the new views use uint16 references; offsets use uint32. Generation fails
before a catalog would overflow 65,536 addressable records.

The snapshot contains 1,509,139 stage/class references and 3,421 directory entries.
Native MSVC sizeof checks report 3,527,042 bytes (3.36 MiB) for the complete
catalog/caps/legacy pool/new views/validation guards, versus approximately 0.46 MiB
before this change. The new view references occupy 3,018,278 bytes. Metadata is
not duplicated per class or stage, and no runtime index allocation is necessary.

Fixed class masks, possible armor/weapon proficiencies across playable races,
and core relic restrictions determine class admission. Shared gear is referenced
by every class that can potentially equip it. Possible proficiency comes from
the hashed `equipment-class-proficiencies.json` DBC snapshot and core skill maps;
it is not a requirement that the bot has already trained the skill. Unknown
proficiencies stay broad. Current learned skills, talents, spec, faction, race,
professions, spells, reputation, unique-equipment rules and scores remain runtime
checks. Previous-era candidates are cumulative under the existing era/stage
guards; unknown acquisition is not promoted to verified provenance.

Only existing fixed failures are removed: unsupported random properties/suffixes,
internal/deprecated items, area/map or honor/city-rank restrictions, no playable
class/race, zero ilvl, and existing stage/era/class impossibilities. Difficulty and
bot-level lower bounds remain runtime filters. Configuration-dependent blacklists
and quality limits are not baked into the index.

Each prep job validates the class-mask/fixed-rejection snapshot against in-memory
server item templates, incrementally, at most 256 records and about 400us per pool
turn. This validation shares the existing soft 3ms search budget; it does not add
a dedicated preparation turn. Until complete, a slot uses the legacy pool for its
entire search. At least 91 validation-bearing turns are required for a compatible
catalog; the 400us limit can require more. Subsequent slots/bots reuse the result
within the same job, without a cross-job cache or runtime SQL query. A mismatch
uses the legacy pool. Additional out-of-profile proficiencies, playable-race
exceptions, or custom armor/relic class contexts also select the legacy pool.
Newly detected proficiency exceptions restart the slot pool before any gear is
applied. This guard assumes item templates are stable during a prep job; a new
job validates again. Hot-changing item templates during preparation is not tested.

Recruitment retains 200ms pacing. Preparation uses its separate global budget;
the per-call 256-entry/eight-evaluation limits remain inside the shared deadline. Validation has its own bounded
record limit inside that time budget. Individual core evaluation calls remain
non-preemptible, as before. Current/previous BiS priority, >5% improvement,
role floors and the original safe bag/swap path are unchanged. There is no early
stop merely because one acceptable item was found: a later candidate may score
higher. This preserves final ordering and selection.

Existing `Candidate` log entries gain `search='index=stage-class16 turns=...
wall_ms=... work_us=... validation_us=... restarts=... exact[...] previous[...]
dungeon[...] ilvl[...]'`. Each phase records `visits`, `scan`, `eval`, `score`,
`wall_ms`, `work_us`, and `stop8/256/time`. `eval` counts detailed evaluator calls;
`score` counts candidates reaching weight calculation after safety/role checks.
Index modes also include `unused`, `legacy-validation-pending`,
`legacy-snapshot-mismatch` and `legacy-class-override`.

A completed bot emits one `Prep timing bot ...` summary for basics, talents,
catalog, spells, skills, plan, gear, AI, verify and save. `wall_ms` includes scheduler
waits; `work_us` is elapsed time inside work calls, not measured CPU utilization.
The search summary excludes final revalidation/equipping; the gear-stage timing
includes it. Logging occurs per completed slot/bot, not per candidate or work turn.

Additional offline commands:

```text
python tools/build_candidate_indexes.py --check
python tools/test_candidate_indexes.py
python tools/analyze_candidate_indexes.py
```

Nine Python regressions cover shared/class-specific gear, cumulative eras, all
3,420 ordered buckets, fixed exclusions, metadata/policy preservation and index
bounds. Native tests compare all buckets, 23,122 snapshot/fallback guards and
20,520 dynamic fixture selections using production comparison/role-floor rules.
These fixtures do not simulate live Playerbots scores or inventory operations;
the original inventory source and safety helpers are pinned by pre-change hashes.

The offline model exhausts both pools, weights all ten classes/nineteen slots
equally, and ignores 3ms stops, validation warm-up, exact/previous BiS successes
and other preparation stages. It predicts pool turns per deep-search slot:

| Profile | Average candidates old -> new | Detailed calls old -> new | Pool turns old -> new |
| --- | ---: | ---: | ---: |
| ZG | 490.05 -> 224.14 | 309.53 -> 124.96 | 40.37 -> 17.03 |
| Karazhan | 797.00 -> 376.96 | 504.63 -> 198.43 | 66.00 -> 26.40 |
| ICC 10 normal | 1414.53 -> 684.53 | 910.53 -> 368.87 | 118.74 -> 48.75 |
| Ruby 25 heroic | 1442.26 -> 699.38 | 951.05 -> 392.22 | 124.21 -> 51.84 |

These are roughly 58-60% fewer modeled pool turns, not a measured raid-wide
speedup. N150 load and full 20-player elapsed time require subsequently authorized
runtime observation. Search limits remain bounded and no new workers, DB queries
or global score caches are added. Score caching across bots/slots is deliberately
avoided because gear, talents and requirements can change during preparation.

Developer setup and current validation status are in [DEVELOPMENT.md](DEVELOPMENT.md).
