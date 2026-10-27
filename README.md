# AzerothCore Raid Manager

Automated raid formation, role balancing, and safe equipment preparation for
Playerbots on AzerothCore 3.3.5a.

Raid Manager was developed to reduce the manual work of assembling a balanced
20- or 40-player raid and preparing its bots for the selected raid tier. It uses
the server's existing Playerbots, talent presets, item templates and equipment
checks. It does not modify combat AI, boss scripts or encounter statistics.

**Compatibility is source-specific.** The validated environment is an archived
AzerothCore/Playerbots source tree with Individual Progression integration, built
with C++20 on Windows x64 using Visual Studio 2026. Upstream commit IDs are not
recoverable from that archive. Stock AzerothCore alone is insufficient, and current
upstream default branches have not been certified. See [COMPATIBILITY.md](COMPATIBILITY.md)
and the [source fingerprints](data/source-compatibility.json) before installing.

## Features

- Form raids of up to 20 or 40 total members, including the player leader.
- Recruit eligible online random Playerbots, balance roles and limit class concentration.
- Optionally promote eligible lower-level bots through the existing level-up path.
- Prepare tank, healer, melee and ranged roles using configured talent presets.
- Learn eligible class trainer spells and prepare already-known combat/weapon skills.
- Select equipment through raid BiS, previous tiers, dungeon and bounded fallback paths.
- Preserve existing equipment and carried inventory through validated storage/swaps.
- Use generated equipment references, incremental searches and per-bot work queues.
- Pace heavy operations and automatically slow preparation when world updates are delayed.
- Cancel queued work and diagnose partial preparation, missing data and unsafe candidates.

The target is **19 bots prepared within 60 seconds**. This is not guaranteed or
claimed as a measured result for the current scheduler. Offline models are not
server benchmarks. Expensive calls, server load and automatic backoff can exceed it.

## Requirements and limitations

You need a compatible AzerothCore checkout, `mod-playerbots`, existing Playerbots
BiS/talent configuration and the core dependencies required by that checkout.
The current shared preparation/promotion gate also requires a compatible, enabled
`mod-individual-progression`, a positive `BotAccountsMaxLevel`, and successful bot
account classification. Missing or incompatible progression APIs fail closed;
compilation without the header does not make promotion or preparation available.
Same-level raid recruitment has a separate path.

Only eligible online random bots are recruited/prepared. This module does not
create accounts, log in offline bots, grant attunements, complete quests, raise
reputation or initialize professions/riding. Human members remain read-only.
Commands require the in-game `RBAC_PERM_COMMAND_GM` permission and are not console commands.

## Installation

From a compatible AzerothCore source root, with its required modules already installed:

```sh
git clone https://github.com/gsehun811-cloud/mod-raid-manager.git modules/mod-raid-manager
```

Keep `src/`, `conf/`, `CMakeLists.txt` and all generated `src/*.inc` files together.
AzerothCore's module loader discovers the module's scripts and loader function.
There is no standalone server executable or module-specific SQL migration here.
The existing Playerbots BiS table must be loaded through Playerbots' own setup.

For a new Unix-like build, follow your core's dependency/setup instructions, then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target worldserver --parallel 2
```

For an already configured Visual Studio build:

```powershell
cmake --build build --config Release --target worldserver --parallel 2 -- /p:CL_MPCount=1
```

Do not clean or replace an existing cache merely to add this module. A newly added
module may require the core's normal CMake regeneration with the same options.
Unix-like module builds have not been validated in the published development snapshot.

Install `conf/raid-manager.conf.dist` as `raid-manager.conf` in the module configuration
directory used by your core. In the validated Windows setup this is
`configs/modules/raid-manager.conf` relative to the server working directory.
Preserve existing configuration and confirm the actual ConfigMgr loading message;
a module log describing an expected path does not prove the file loaded.

Stop your own server before replacing its executable. Restart it after installing
the newly built module. This repository does not deploy files, control servers or
change databases automatically.

## Commands

| Command | Implemented behavior |
| --- | --- |
| `.raidgroup 20` | Recruit eligible same-level bots toward 20 total raid members. |
| `.raidgroup 40` | Recruit eligible same-level bots toward 40 total raid members. |
| `.raidgroup 20 fill` | Fill toward 20, also considering eligible lower-level bots for promotion. |
| `.raidgroup 40 fill` | Fill toward 40, also considering eligible lower-level bots for promotion. |
| `.raidgroup cancel` | Stop this leader's recruitment; completed joins/promotions remain. |
| `.raidprep` | Queue preparation of owned random bots in the leader's actual raid instance. |
| `.raidprep cancel` | Stop new preparation work; retain completed changes and drain persistence/reservations. |
| `.raiddiff 1` through `.raiddiff 7` | Registered prototype; difficulty effects are not implemented. |

`.raiddiff` does not grant higher-tier gear, buffs or encounter changes. Its level-1
baseline acknowledgement is not proof that all bots finished preparation.

### Recruitment

The caller must be an available leader of a normal group, or have no group. Pending
invites, conflicting group reservations, battleground/LFG groups and unsuitable
player states are rejected. Formation can be requested outside a raid. Inside a
raid, the requested size cannot exceed its actual capacity.

Candidates must be online, available, same-faction random bots without conflicting
masters, groups/invites or instance bindings. Existing members are retained.
The role targets are 2 tanks/5 healers for 20, and 4 tanks/10 healers for 40. Roles
are taken from current specs. Class, support-vacancy and melee quotas favor balance
and diversity; these limits can intentionally leave vacancies in a skewed pool.
Recruitment times out after five minutes rather than forcing unsuitable members.

`fill` prioritizes role needs and class diversity, then same-level candidates and
smaller level gaps. Promotion respects server, Playerbots and progression limits.
`RaidManager.MaxPromotionsPerRun = 0` means no numeric attempt cap, still bounded
by vacancies, eligibility and timeout. Positive values count attempts, including
failures. Same-level recruitment does not consume promotion attempts.

Promotion is permanent and normal Playerbots level-up maintenance remains enabled;
it may update gear/spells/talents independently. The strict item-preservation path
described below belongs to `raidprep`, not every underlying Playerbots maintenance
operation. Cancellation does not roll back promotions.

### Raid preparation

The caller must lead a raid inside a real raid instance, out of combat. Actual map,
instance, difficulty and capacity are checked. Only owned, living, out-of-combat,
non-teleporting random bots in that same instance are prepared. Human members,
other leaders' bots and incompatible instance bindings are not changed.

Stages cover basics, validated role talents, class trainer learning, existing skills,
gear planning/slots, strategy inspection, final verification and normal persistence.
Existing tanks/healers are preserved; missing support roles are assigned subject
to verified presets. Feral bear/cat presets must be distinguished explicitly.
The broad DK spell helper is excluded because it includes mount/starting-area effects.
Existing Commander/custom strategies are inspected, not replaced or rewritten.

### Example: 20-player Zul'Gurub

Use an eligible level-60 leader with enough online same-faction bots and verified
progression limits. Form the group outside the raid if appropriate:

```text
.raidgroup 20 fill
```

Wait for recruitment to finish and review vacancies/promotion reports. Get the
leader and owned bots into the same Zul'Gurub instance using your normal server
workflow. Then run:

```text
.raidprep
```

Review per-bot and final summaries. Use `.raidprep cancel` if necessary. Summoning,
teleport and repair behavior still depends on the existing Playerbots settings.

## Configuration

The checked-in file is a template, not a private realm configuration.

| Setting | Meaning |
| --- | --- |
| `RaidManager.MaxPromotionsPerRun` | Fill promotion attempts; default 0, negative values treated as 0. |
| `RaidManager.Prep.BisKey.<map>` | Override an existing map's exact BiS cache key; 0 disables its gear preparation. |
| `RaidManager.Prep.Profiles` | Semicolon-separated `map difficulty capacity name bisCacheKey` exact profiles. |
| `RaidManager.Prep.Spec.<class>.<role>` | Exact Playerbots preset name; role is Tank/Healer/Ranged/Melee. |

Exact profiles override map keys, which override automatic defaults. Duplicate exact
profiles warn and keep the first. Do not define conflicting duplicate configuration
keys. Config changes take effect through the core's startup/config reload behavior;
active promotion limits are snapshots.

Defaults distinguish Classic, TBC and Wrath profiles. For example, Zul'Gurub uses
BiS key 78, ICC 264 and Ruby Sanctum 290. **A key is a lookup label, not an ilvl cap.**
Custom Classic Onyxia/Naxx and unknown variants do not inherit Wrath loot evidence.

## Equipment selection

For each required slot, the implemented priority is:

1. Current raid/class/spec/faction BiS.
2. Previous raid-stage BiS in explicit progression order.
3. Verified dungeon candidates.
4. Ilvl-bounded fallback candidates, including unverified acquisition sources.
5. Keep existing equipment if no safe improvement exists.

A valid already-equipped exact BiS is retained. A usable exact BiS that fails the
improvement test also retains existing equipment. Previous-tier lookup is not
arithmetic subtraction from a key. Missing specialization/slot rows do not stop
the later candidate paths.

Every candidate still needs slot, class, actual spec/role, faction/race, required
level, proficiency, spell, reputation and core equip compatibility. Playerbots
quality/configuration limits remain authoritative. Known later stages and blocked
same-stage raid difficulties are rejected even when ilvl is low. Caps use actual
item anchors and source associations, not BiS key numbers.

Unknown acquisition may pass the bounded fallback but never becomes source proof.
Conservative required-level and item-ID era exclusions reduce inappropriate
cross-expansion items; they cannot prove historical introduction or detect every
low-ID backport. See [EQUIPMENT_FALLBACK.md](EQUIPMENT_FALLBACK.md) and
[SOURCE_VALIDATION.md](SOURCE_VALIDATION.md) for exact evidence limits.

An upgrade must have a finite positive score exceeding the old score by more than
5% plus the implementation epsilon. Tank stamina/armor/defense/avoidance/block,
healer healing/mana regeneration/intellect/spirit, relevant hit/expertise/spell
power and weapon DPS have additional non-regression floors. Actual existing
enchants/gems count; speculative empty sockets and new set bonuses do not.

Core checks enforce weapon combinations, talents, unique-equipped restrictions,
unequipping and storage. Coupled weapon swaps that would displace an unscored
offhand are deferred. Random-property/suffix candidates and speculative replacement
of existing random/set gear are conservatively deferred.

Existing matching bag items may be reused. Old equipment must fit safely in carried
bags before a new item is staged. Nothing is deleted to make room. If an equip
fails, the newly staged item remains in bags. Full bags can skip the slot. The
module does not add a permanent gear lock against later Playerbots maintenance.

## Precomputed equipment and performance

Development-time generators produce the data checked into `data/` and `src/*.inc`.
Normal server compilation includes those arrays in the binary; CMake does not
automatically run Python or regenerate them. There are no runtime SQL scans of
23,122 candidates and no filesystem JSON reads by the equipment selector.

The bundle includes 18-stage/class/slot/ilvl views with 16-bit references, 36 cap
profiles, 2,304 profile/spec/role/faction sets, 818 merged BiS records and up to
four advisory slot candidates per set. Shared equipment is available to every
compatible class. Previous-era candidates are cumulative under the existing gates.

Compiled current/previous BiS references must agree with current cached inputs.
Actual spec/role/profile mismatches safely use the original path. SQL stat-based
suggestions are not final runtime scores. Full fallback comparison remains necessary
because current ratings, auras, talents, weapons and old equipment can change the
best result. The strict policy does not immediately equip the first usable suggestion.

Recruitment keeps its 200ms admission clock. Preparation uses a separate global
30,000us/s token budget with a 3,000us burst, normally admitting work every 50ms.
It batches at most 32 resumable microsteps; searches retain per-call 256-entry/eight-
evaluation limits and share the slice deadline. Per-bot queues rotate independent
work while serial role/talent commitment prevents duplicate support-role allocation.
Heavy transitions are not piled into one slice. Full elapsed overruns become debt.

World-update delays of at least 100ms or 250ms reduce the budget/pace for two seconds.
Atomic core calls cannot be preempted, so the time limit is soft and load can extend
completion. No worker threads or unlimited per-bot budgets are added. See
[PREPARATION_PERFORMANCE.md](PREPARATION_PERFORMANCE.md) for bounds and log fields.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No candidates or unfilled group | Bot availability, masters/groups, bindings, quotas and timeout report. |
| Promotion/preparation blocked | Progression API/enabled state, bot account classification and all three level caps. |
| Partial preparation | Per-bot categories, presets, role spells, bot/instance state and active strategies. |
| Missing exact BiS/spec data | Key/class/tab/faction and cached slot count; fallback can still run. Bear uses tab 10. |
| No equipment upgrade | Role floors, >5%, preserved sets/random gear, quality cap and core restrictions. |
| Bag-space or unique conflict | Keep room for old gear and staging; conflicting slots are not forcibly reordered. |
| Slow preparation | Stage/search logs, snapshot fallback, expensive calls, contention and backoff. |
| Snapshot mismatch | First mismatching item and expected/actual fields; broad search remains enabled safely. |
| Config appears ignored | Working directory, ConfigMgr load report, filename and profile priority. |

The log category is `module.raidmanager`. Relevant messages include `Candidate`,
`Candidate index snapshot mismatch`, `Prep timing bot`, `Prep equipment bot` and
`Prep job timing`. Work microseconds are elapsed call time, not OS CPU utilization.
Some budget counters are service-wide lifetime counters, not exclusive per-job values.

## Development, tests and provenance

Generated data is sufficient to compile and operate the module. Compatible external
core SQL/headers are needed only to regenerate it and run source-backed regressions.
No game client DBC files, databases or private server settings are distributed.

```sh
python tools/verify_bundle.py
python tools/test_source_manifest.py
```

For full regressions, set `RAID_MANAGER_SOURCE_ROOT` to a compatible reference
checkout and use `tools/run_offline_tests.py`, or `tools/run_candidate_tests.ps1`
on Windows. See [DEVELOPMENT.md](DEVELOPMENT.md) for reproducibility and compiler setup.

The development snapshot passed a Windows Release build and 34 Python/39 native
regression checks. Packaging revalidates unchanged runtime files and the generated
bundle. Offline checks do not establish live 19-bot completion time, all-raid boss
readiness, or compatibility with every upstream branch.

## Status and license

Working features are listed above. Difficulty effects, higher-tier `.raiddiff`
selection/buffs, universal historical loot verification and guaranteed 60-second
performance are not implemented or certified. See [CHANGELOG.md](CHANGELOG.md).

Raid Manager is distributed under **GPL-2.0-or-later**; see [LICENSE](LICENSE),
[NOTICE.md](NOTICE.md) and [AUTHORS.md](AUTHORS.md). Upstream data and dependencies
retain their own notices, including file-specific GPL/AGPL terms. This repository
does not relicense AzerothCore, Playerbots, client assets or the game.
