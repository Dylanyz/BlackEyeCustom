# BlackEyeCustom: Black Eye extensions, written for the Black Eye devs to port

A C++ UE 5.8 editor plugin by Dylan Gitalis (Mad Rice): the home for our custom extensions to the Black Eye camera
plugin (Fab, 2.0.7). Dylan shares this repo with Black Eye's developers so they can rebuild each extension natively;
the repo *is* that handoff. First extension: **Fast Bake** (`docs/fast-bake/DESIGN.md`). Private, no remote yet ·
CPAL-1.0 + Commons Clause, plus a permission for Black Eye Technologies (`.claude/rules/licensing-and-credits.md`).

**This repo is the installed plugin.** `Engine\Plugins\Marketplace\BlackEyeCustom` is a junction to this folder. Docs
are live; C++ needs a build and an editor restart. Build, install, Live Coding, licensing and wrap-up are the plugin
hub's (`../CLAUDE.md`, `../.claude/refs/`); this file holds only what is BlackEyeCustom's.

## Hard rules

`.claude/rules/blackeye-handoff.md` auto-loads: cite never paste, every Black Eye name through `BlackEyeContract`,
`BE-NATIVE` on every workaround, DESIGN updated with the code, no film names. Also:

1. **Extension, not a fork, and no link to `Black_Eye`** (DESIGN §9). Never patch Black Eye's install.
2. **Black Eye facts:** `/ue-blackeye` (manual, source location, known issues). Read its
   `references/extending-blackeye.md` before building on Black Eye.
3. **Editor work:** `/ue-agent-control` first. Plugin tests run in the tester project, not the film in production.

## Start here

- **Current state, the open issue, next steps → `.claude/refs/status.md` (read first)**
- What Fast Bake does, why, the algorithm, traps, decisions, phases → `docs/fast-bake/DESIGN.md`
- Every Black Eye symbol used, and the self-test → `Source/BlackEyeCustomEditor/Public/BlackEyeContract.h`
- Repo-specific maintenance, the skill/repo knowledge split, Black Eye update checklist → `.claude/refs/maintenance.md`
- Public explanation → `README.md`

## Iterating

Start with `Tools\build_blackeyecustom.ps1 -Status`.

| Change | Then |
|---|---|
| Docs, `README.md`, `.claude/` | commit |
| Anything in `Source/` | build, install (hub `refs/build-install.md`), relaunch, `BlackEyeCustom.SelfTest`; DESIGN in the same commit |

**Self-test:** `BlackEyeCustom.SelfTest` in the console; every line `ok`, last line `PASSED`. A project must enable
the plugin (Edit ▸ Plugins), or pass `-EnablePlugins=BlackEyeCustom`. Headless, from plain PowerShell:
`UnrealEditor-Cmd.exe <tester.uproject> -run=pythonscript -script=<file running the command> -EnablePlugins=BlackEyeCustom -stdout -unattended -nosplash -nullrhi`,
then grep the output for `[BlackEyeCustom]`. The commandlet's own exit code reflects unrelated project load errors;
judge by the `PASSED` line. (Verified 2026-10-06.)

**Testing Fast Bake** (tester project, repro at `/Game/Claude/FastBake/`): launch the tester with
`-DisablePlugins=MetaHumanCrowdContent,MovieSceneAnimMixer` or every subject freezes (DESIGN trap 4.11). Bake and
record from Python (README "Reproduce"); `BlackEyeCustom.FastBake.Verbose 1` logs each subject mesh's anim state.
Realtime records need "Use Less CPU when in Background" off (`bThrottleCPUWhenNotForeground` on
`/Script/UnrealEd.Default__EditorPerformanceSettings`), and the bake must use `SubSteps` = editor fps / sequence fps
to match one (trap 4.12).

## Layout

| Path | What |
|---|---|
| `BlackEyeCustom.uplugin` | one `Editor` module, Win64, `Installed: true`, no Black Eye dependency |
| `Source/BlackEyeCustomEditor/` | `BlackEyeContract` (every Black Eye name + `RunSelfTest`), module + console command. Each extension adds a `Private/<Extension>/` folder |
| `Source/.../Private/FastBake/`, `Public/BlackEyeFastBakeLibrary.h` | `BlackEyeFastBake.cpp` the bake loop (`RunBake`); `BlackEyeFastBakeTwin.cpp` the twin, lock/unlock, bake info; `BlackEyeFastBakeMenu.cpp` the Sequencer menu and console commands; library: `BakeShot`, `SetLocked`, `GetBakeInfo`, `BakeCameraToCsv`, `Start/StopRealtimeRecord` |
| `Tools/fast_bake_repro.py`, `Tools/compare_bake.py` | builds the repro scene in any project; compares baked vs realtime tracks (plain Python) |
| `docs/<extension>/DESIGN.md` | one living design doc per extension; `docs/<extension>/data/` for measurements |
| `Tools/` | `build_blackeyecustom.ps1` (package `%TEMP%\bcb`), `install_junction.ps1` |

## Adding an extension

New `docs/<name>/DESIGN.md` (same status-per-section shape), `Source/.../Private/<Name>/`, a row in the README's
Extensions table, new Black Eye names in `BlackEyeContract` with self-test checks.
