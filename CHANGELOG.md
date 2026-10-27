# Changelog

## Initial publication snapshot (unversioned)

This is the first standalone publication of the existing module, not an invented
sequence of versioned releases. Runtime C++, configuration and generated arrays
are preserved from the validated development snapshot.

### Implemented

- Same-level 20/40-member raid recruitment, balanced fill/promotion and cancellation.
- Role-aware preparation, guarded talents/class spells/existing combat skills.
- Source-evidence diagnostics and explicit map/stage/difficulty policies.
- Current/previous BiS, dungeon and bounded unverified fallback with safe retention.
- Class/stage/slot/ilvl views, shared uint16 references and strict generated sets.
- Global preparation time budget, fair bot queues, paced heavy work and adaptive backoff.
- Source-backed regressions, policy models and generated-data checks.

### Publication work

- English installation, compatibility, equipment, performance and provenance documentation.
- Portable developer-tool reference paths and compiler setup; standalone bundle verification.
- Clean source-only package with GPL text, upstream author notices and an exclusion-oriented gitignore.

### Known limits

- `.raiddiff` is registered but its effects are not implemented.
- The latest 60-second target has not been established by a live N150 benchmark.
- Full fallback ranking remains to preserve results; suggestions cannot replace live scores.
- Missing/unverified source data and conservative role floors may leave equipment unchanged.
- Current promotion/preparation require the compatible progression/account-classification integration.
- Validation used an archived source tree without recoverable upstream commit IDs.
- Windows Release compilation is validated; other platforms/branches need integration testing.
- No publication step changes gameplay, other modules, databases or a running server.
