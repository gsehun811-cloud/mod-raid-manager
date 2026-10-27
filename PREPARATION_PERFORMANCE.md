# Strict generated sets and bounded preparation

Target: one player plus nineteen bots finish preparation within sixty seconds on
N150 with other Playerbots active. This is a live acceptance target, not an offline
guarantee. The published runtime snapshot passed a Windows Release compile/link. Offline
policy tests do not run a server or connect to a DB; see DEVELOPMENT.md.

## Generated sets and result preservation

`tools/build_preparation_sets.py` reads existing SQL/catalog/proficiency data and
generates `data/preparation-sets.json` and `src/RaidPreparationSetData.inc`:

- 2,304 map/difficulty/capacity/class/spec/role/faction sets, including bear tab 10
  and blood DK tank/DPS keys; 818 faction-merged BiS records.
- Up to four advisory fallback references per slot, ordered by explicit SQL stat
  weights/weapon damage for the class/spec/role. These are not actual live scores.
- uint16 references address the one original 23,122-entry catalog. Masks/counts
  distinguish absent values from valid index zero or 65,535. No item metadata or
  character-state permutations are duplicated.

MSVC sizeof: 475,688 additional bytes for sets/BiS records; original catalog/index
arrays remain 3,527,042 bytes; combined static data is 4,002,730 bytes.

The selector queries actual profile, tab, role and faction on every step. Current
BiS references must equal the original exact lookup. Previous BiS records must
equal the entire current in-memory BisListMgr merge, otherwise original live values
are used. Overrides, unknown profiles, missing references and incompatible roles
fall back safely. No runtime SQL query or cross-bot score cache is added.

Hints are recognized at their original positions in full pool traversal. They do
not reorder evaluations or stop the search. Every original score, source/era/cap
restriction, role floor, core wear check, >5% improvement comparison and final
revalidation remains. ApplyPreparedItem is untouched. Small lists cannot replace
all live scoring while also guaranteeing the same maximum-score result.

Equivalence assumes identical character/config/data inputs. Shorter elapsed time
can expose different live auras or requirements, so static tests cannot guarantee
identical item IDs across changing live states.

## Snapshot mismatch

The first mismatch per job logs item ID, catalog ordinal, template presence,
expected/actual playable-class masks and fixed-rejection status, random property/
suffix, area/map, city/honor rank and internal-item diagnostics. It retains the
broad exhaustive legacy pool. No protection is disabled; existing gear is preserved.
Job completion logs the mismatching ID. The particular failing live item remains
unknown until the new diagnostic runs; this revision does not guess or patch SQL.

## Scheduling bounds

Recruitment retains its own 200ms clock, original actions and reservation registry.
Preparation no longer consumes that clock and yields while recruitment is due or
has just run. One global budget covers every prep job, including forty-player raids.

| Observed world diff | Hold | Minimum interval | Maximum slice |
| --- | --- | --- | --- |
| Below 100ms | normal | 50ms | 3,000us |
| At least 100ms | 2 seconds | 100ms | 1,500us |
| At least 250ms | 2 seconds | 250ms | 500us |

Credit refills at 30,000us/s with a 3,000us burst cap. A slice runs at most 32
resumable microsteps. Each pool call retains its 256-entry/eight-evaluation limits
and shares the outer deadline. There are no new workers or per-bot CPU allowances.
Overload may make preparation exceed sixty seconds; work is not skipped to finish.

Only one heavy transition (basics, talents, equip, save or failure persistence)
runs in a slice. Core calls cannot be preempted. The complete elapsed cost, including
overruns, is charged as debt and delays later admissions. The deadline is soft;
maximum-call/slice measurements must be reviewed live. Existing exception-time
persistence is retained.

Skills check at most eight entries per microstep. Trainer learning checks at most
32 rows per microstep and stops advancing rows after eight newly learned spells
in a slice. A simple row's up to three effects finishes atomically, so its final
row may exceed that soft threshold by two. Existing class/level/prerequisite and
profession/riding guards remain unchanged.

Trainer discovery preserves original trainer/spell order to avoid prerequisite
changes. Its 512-entry chunks now share a slice instead of each waiting 200ms.
The original one-time template-ID copy remains non-preemptible; it was part of
about 2.9ms of catalog work in the inspected log. No new full-store scan is added.

Role assignment/talent commitment is serial in member order so another bot cannot
reserve the same apparent tank/healer deficit. Independent bot contexts and jobs
then rotate fairly. No Player/AI pointer survives a turn; gear slot order remains.
Cancellation admits no new bot, persists/releases queued contexts one heavy turn
at a time, then reports/releases the group. Epoch, membership, leader/instance and
combat checks stay active. No inventory delete/clear/rollback helper is introduced.

## Logs and acceptance

- Candidate search adds `sets=strict`, `preparedHits`, `preparedFallback` and
  `advisoryHints`; those counters do not promise fewer weight evaluations.
- `Prep equipment bot` records apply attempts, retained choices and admission/
  request-to-done wall time. Existing stage/search summaries remain.
- `Prep job timing` measures request acceptance through final drained completion,
  with partial/failed/skipped counts, microsteps, work/max-step time, mismatch item,
  and coarse world-diff p95/p99 upper bounds/max. Budget counters/max-slice are
  global service lifetime counters, not exclusive per-job counters.
- `work_us` is elapsed call time, not OS CPU utilization. World-diff upper buckets
  are 5/10/20/30/50/75/100/150/250ms followed by the observed maximum.

Compare cold/warm runs separately without deleting gear, resetting DBs or using
old backups. Extra skips/weaker verification do not count as success. N150 live
measurement remains user-operated; this development does not execute raid commands.

## Offline validation

```text
python tools/build_preparation_sets.py --check
python tools/test_preparation_sets.py
powershell -File tools/run_candidate_tests.ps1
```

Independent SQL merges, class/faction/progress references, profile/spec/role and
changed-cache mismatches are checked alongside all existing 3,420 pool-order and
20,520 dynamic fixture result comparisons. Original inventory, recruitment and
role/scoring/protection fingerprints remain checked. Historical RaidWork/LearnStep
pins are explicitly excluded because these are the approved scheduling/batching
changes; the original baseline file is not rewritten.

Native tests execute production budget/queue/set APIs for debt, lag/recovery,
serial role commitment, 19/39 contexts, cancellation and reservation bookkeeping.
They do not emulate live core inventory, talent hooks or persistence. A synthetic
workload per bot (600 light 80us steps, 450us basics, 14ms talents, 2ms save and
80us admission) yields 40.754s for nineteen and 83.774s for thirty-nine. Transient
lag yields 41.282s for nineteen. These are assumed-cost policy models, not N150
measurements or forecasts. Expensive live evaluation, atomic calls, contention,
changing auras and automatic backoff remain risks.

`tools/check_preparation_syntax.py` runs MSVC /Zs using the last approved build's
flags from an explicitly supplied --build-log for four production sources. It does not invoke CMake/MSBuild or generate
worldserver. Release linking passed in the validated development environment; live initialization
and timing still require acceptance testing.

Developer setup, portable tool paths and validation limits are documented in
[DEVELOPMENT.md](DEVELOPMENT.md). Runtime code and data are unchanged by publication.
