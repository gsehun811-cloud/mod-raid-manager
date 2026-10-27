# Source compatibility

## Validated environment

The runtime snapshot passed a Release worldserver compile/link on Windows x64,
Visual Studio 2026 and C++20 against an archived AzerothCore/Playerbots tree.
That archive reports an unknown Git revision; no exact upstream commit or tag is
claimed. Compare the file SHA256 values in data/source-compatibility.json and the
generator input fingerprints before assuming an identical environment.

This is not a core patch or an independent worldserver. Upstream project links
identify dependencies, not certification of their latest branches. Linux and
other current revisions have not been integration-tested for this publication.

## Required interfaces

- AzerothCore's module loader, ChatCommandTable/RBAC API and World/Player/Group hooks.
- Playerbots' PlayerbotOperation/PlayerbotOperations and PlayerbotWorldThreadProcessor.
- RandomPlayerbotMgr ownership/online bot iteration, PlayerbotAI role APIs and AI factory spec tabs.
- BisListMgr::GetBisFor with Both/faction merging and druid bear sentinel tab 10.
- StatsWeightCalculator and StatsCollector settings/methods used by the selector.
- PlayerbotAIConfig premade spec/talent ordering, level and quality options.
- Compatible IndividualProgression.h exposing enabled, BotAccountsMaxLevel and isBotAccount.

Missing Playerbots interfaces are compile-time compatibility errors. The optional
header probe for IndividualProgression is intentionally different: absence can
compile, but the promotion/preparation gate remains unavailable. It must not be
bypassed just to make a different server accept the command.

## Data and configuration

Existing Playerbots BiS data and configured talent presets are runtime inputs.
This repository does not create their table or install their SQL. BiS keys are
exact labels, not ilvl ceilings. Custom keys/presets use runtime validation and
may fall back to the exhaustive path. Quality limits, account/server level caps
and core equipment requirements remain authoritative.

The world/source snapshot used by the generators is not a merged live database.
Input fingerprints cover only named source files, not every API, SQL update or
custom hook. Hash equality is useful evidence, not a universal ABI guarantee.
Different templates safely trigger diagnostics and broader search, which can be slower.

## Database behavior

Offline tools read source SQL as text and never connect to MySQL. The module adds
no schema migration and its equipment search adds no SQL scan. Normal runtime
level/preparation changes still use core character persistence; the existing
progression bot-account classifier can read the login database. Do not interpret
offline no-DB testing as a claim that the running server never accesses its DB.

## Inventory and external maintenance

The raidprep swap/storage helper preserves old gear and carried contents. Fill
promotion invokes normal Playerbots level-up maintenance, which can independently
change equipment and spells. Later maintenance may replace prepared gear; there
is no permanent equipment lock. Banks and unrelated characters are not swept.
