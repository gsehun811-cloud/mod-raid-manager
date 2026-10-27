# Development and reproducibility

## Build versus regeneration

Checked-in src/*.inc files are the production equipment data. They are included
by the normal C++ module build. Python, source SQL, evidence JSON and a client DBC
are not read at server runtime. Keep all four generated includes:

- RaidEquipmentSourcesData.inc
- RaidEquipmentCandidateData.inc
- RaidEquipmentCandidateIndexes.inc
- RaidPreparationSetData.inc

The corresponding JSON files preserve evidence and generator inputs. They are
included for inspection/reproduction, not installed into a database. No EXE/DLL,
CMake cache, runtime configuration or server data belongs in this repository.

## Reference checkout

Generation and source-backed tests require a compatible AzerothCore checkout with
its source SQL, scripts and matching Playerbots BiS SQL. Use the source hashes in
the JSON manifests and COMPATIBILITY.md. Cloning only this module is enough for
bundle verification, but not enough to recreate data derived from external SQL.

When installed as modules/mod-raid-manager, tools can discover the core root.
For an independent clone, set a reference checkout explicitly:

```sh
export RAID_MANAGER_SOURCE_ROOT=/path/to/compatible/azerothcore
```

```powershell
$env:RAID_MANAGER_SOURCE_ROOT = 'D:/source/compatible-azerothcore'
```

Tools read that checkout but write only this module's generated files/test output.
No database access or server execution is involved. Mismatched or unsupported SQL
layouts fail rather than silently omitting evidence.

## Generation order

Run from the module root:

```sh
python tools/build_source_manifest.py --check
python tools/build_candidate_catalog.py --check
python tools/build_candidate_indexes.py --check
python tools/build_preparation_sets.py --check
```

Omit --check to regenerate, in the same order, after reviewing source/policy changes.
The bundled proficiency seed records the hash and extracted factual class/skill
masks; the game client's DBC itself is not redistributed. --skill-dbc is an explicit
optional seed import and is not needed to reproduce with the bundled seed. Never
infer historical acquisition solely from an item ID, ilvl or BiS key.

## Tests

No reference checkout or compiler is needed for these checks:

```sh
python tools/verify_bundle.py
python tools/test_source_manifest.py
```

For source-backed Python checks only:

```sh
python tools/run_offline_tests.py --python-only
```

For native rules/policy regressions in a VS developer shell:

```powershell
python tools/run_offline_tests.py
```

Alternatively, PowerShell discovers installed Visual Studio C++ tools via vswhere:

```powershell
./tools/run_candidate_tests.ps1 -SourceRoot D:/source/compatible-azerothcore
```

The Windows runner uses /MP1 and /W4 /WX. A C++20 compiler can be selected with
--compiler; the Unix-like command path is provided but has not been validated on
Linux for this publication. Native tests compile standalone rules and models,
not worldserver, then write only ignored tools/.build outputs.

The full suite has 34 Python checks and 39 native top-level checks, including
3,420 ordered pool comparisons, 20,520 fixture selections, 818 independent BiS
merges, budget/debt/backoff, serial role commitment, 19/39 queues and cancellation.
Game inventory, AI hooks and actual character persistence are not emulated.

The optional Windows syntax checker accepts an explicit compatible MSVC build log:

```powershell
python tools/check_preparation_syntax.py --build-log D:/build/worldserver-build.log
```

It uses that log's compiler flags with /Zs for four module sources, without running
CMake/MSBuild or generating a server. The log is a trusted local development input;
do not use an untrusted build log as executable compiler configuration.

## Validation status

The runtime files in this publication are byte-identical to the development
snapshot that passed a Windows Release compile/link with zero reported errors or
warnings. Packaging modifies documentation/developer tooling only. It does not
perform a new worldserver build or a server/DB test. tools/verify_bundle.py checks
release file fingerprints and generated-array integrity.

Synthetic policy times (about 40.8s for nineteen contexts under assumed costs) are
not N150 measurements. The current scheduler still needs live acceptance testing,
especially under many bots, cold caches, changing auras and long atomic calls.
