---
name: game-update-offsets
description: Refresh and synchronize BUNNYWARE offsets after a Counter-Strike 2 game update. Use when live CS2 offsets need to be regenerated, checked against the running process, written to offsets.json, copied into build/, or committed and pushed without including unrelated worktree files.
---

# Game Update Offsets

Refresh BUNNYWARE's consolidated `offsets.json` from a live CS2 process after a game update. The refresh runs in-process, so no external dumper is required: base offsets, the convar interface, and the schema fields BUNNYWARE consumes are all re-resolved automatically.

## Built-in refresher (normal path)

BUNNYWARE refreshes the offsets it consumes directly from the running `cs2`
process on every launch, before `offsets.json` is loaded. Everything is read with
`process_vm_readv` only (module discovery uses `/proc/<pid>/maps`).

- Implementation: `src/overlay/offset_refresher.{hpp,cpp}` with the pure
  pattern/operation helpers in `src/overlay/offset_scanner.hpp`.
- Scope (only what BUNNYWARE consumes):
  - 12 signature-derived base offsets (`dwEntityList`, `dwViewMatrix`,
    `dwLocalPlayerController`, `dwLocalPlayerPawn`, `dwGlobalVars`,
    `dwCSGOInput`, `dwViewAngles`, `dwSensitivity`,
    `dwSensitivity_sensitivity`, `dwWindowWidth`, `dwWindowHeight`,
    `dwGameRules`).
  - The `tier0_VEngineCvar` interface fallback (the convar reader also resolves
    ICvar dynamically at attach time).
  - The schema fields requested by `get_schema()` in
    `src/overlay/game_reader.cpp` (~100 entries listed in `kSchemaKeys`). All
    other `interfaces`/`schemas` entries are preserved untouched.
- Merge behavior: values are updated in place, unknown keys are preserved, and
  the file is only rewritten when at least one refreshed value changed. Synced
  copies (`build/offsets.json`, or the source copy when running from `build/`)
  are updated together and file permissions are preserved.
- Validation and reporting:
  - Every required signature must resolve; otherwise nothing is written, the
    old `offsets.json` is kept, and each failure is logged
    (`signature not found: <name> in <module>`).
  - The refresh also performs live sanity reads (view matrix, entity list,
    window size, ICvar list) and reports them as warnings.
  - Schema fields that no longer exist in the build are reported as warnings;
    their previous values are kept.
  - `bunnyware --update-offsets` runs only the refresh (no overlay) and exits
    non-zero when signature validation fails, which is handy for scripts and
    for testing after a game update.
- When a signature pattern itself changes after a game update (all other
  patterns resolve but one is reported missing), update the pattern in
  `signature_definitions()` in `src/overlay/offset_refresher.cpp` and rebuild.

If CS2 is not running when the overlay starts, the refresh is skipped and the
existing `offsets.json` is used; start CS2 before the overlay to refresh.

## Paths and formats

- BUNNYWARE source: `offsets.json`
- Runtime copy: `build/offsets.json`

BUNNYWARE expects the consolidated format:

```json
{
  "offsets": {},
  "interfaces": {},
  "schemas": {}
}
```

## Workflow

### 1. Inspect and preserve the worktree

Run `git status --short --branch` and inspect the existing diff before editing. Stage only the intended offset and skill files; leave unrelated source changes, generated build artifacts, and cache files alone.

### 2. Refresh from the live process

Ensure CS2 is running, then run the refresher on its own:

```bash
./build/bunnyware --update-offsets
```

`--refresh-offsets` is accepted as an alias. The command exits non-zero when a required signature fails to resolve; treat that as a blocker and do not manufacture values from the previous update.

### 3. Review the report

Confirm the refresh reported every base offset resolved and that no consumed schema field was dropped. A missing schema field is a warning and its previous value is kept; a missing base offset means nothing was written and the old `offsets.json` is still in place.

### 4. Synchronize and verify the build copy

The refresh rewrites `build/offsets.json` together with the source `offsets.json` when it runs from the repository root. If the files were edited or copied by hand, re-sync and verify both parse and are byte-identical:

```bash
cp offsets.json build/offsets.json
python - <<'PY'
import hashlib, json
from pathlib import Path

source = Path("offsets.json")
build = Path("build/offsets.json")
for path in (source, build):
    json.loads(path.read_text())
    print(path, hashlib.sha256(path.read_bytes()).hexdigest())
assert source.read_bytes() == build.read_bytes()
PY
git diff --check
```

Run the C++ build and CTest when the offset update is part of a code change or when runtime confidence is needed. An offset-only edit should at minimum pass JSON parsing, source/build identity, and live validation.

### 5. Commit and push intentionally

Fetch and push with the configured SSH key without exposing its contents:

```bash
git -c core.sshCommand='ssh -i $HOME/.ssh/id_ed25519 -o IdentitiesOnly=yes' fetch origin
git add offsets.json skills/game-update-offsets/SKILL.md skills/game-update-offsets/agents/openai.yaml
git commit -m "chore: update offsets after CS2 game update"
git -c core.sshCommand='ssh -i $HOME/.ssh/id_ed25519 -o IdentitiesOnly=yes' push origin main
```

Do not stage `build/offsets.json` unless it is tracked; it is normally ignored and is a generated runtime copy. After pushing, verify `HEAD...origin/main` is `0 0` and report the commit hash, validation result, changed offset groups, and any unresolved warnings.
