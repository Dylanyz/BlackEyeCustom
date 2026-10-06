# Maintenance: BlackEyeCustom only

The wrap-up checklist shared by every plugin is the plugin hub's `refs/maintenance.md`; run it first. This file
holds what is specific to this repo.

## Wrap-up, this repo

- Code changed → the extension's `docs/<extension>/DESIGN.md` changes in the same commit (section status, traps,
  numbers). New workaround → a `BE-NATIVE` comment and, if it's a Black Eye bug we work around, a row in
  `/ue-blackeye` `references/extending-blackeye.md` § Custom fixes (pointer only; the detail stays here).
- New Black Eye symbol → `BlackEyeContract.h/.cpp` plus a self-test check.
- Measured something → `docs/<extension>/data/` (small CSV/plot) and the DESIGN numbers table.

## Where knowledge goes (this repo vs `/ue-blackeye`)

- **This repo** is the single source of truth for each extension: how it works, why, traps, numbers.
- **`/ue-blackeye`** keeps a pointer to this repo plus Black Eye facts that stand on their own (true without this
  plugin, e.g. "2.0.7 has no Bake button", "the editor only fires `NotifyCameraCut` on a jump or actor change").
- **`/ue-docs`** gets engine-true facts (AutoBake / `FSequencerBaker`, Bake Transform limits).
- **The film project's `.claude/refs/`** gets which shots, edits and numbers that film used.
- If Black Eye ships a feature natively, retire that extension with a pointer and switch `/ue-blackeye` to their docs.

## When Black Eye updates (Fab)

1. `BlackEyeCustom.SelfTest`: every line `ok`. A FAIL names the renamed symbol.
2. Bump `BlackEyeContract::TestedVersion` only after the checks below.
3. Re-check every `file:line` citation (`grep -rn "\.cpp:\|\.h:" docs Source`) against the new source; the
   citations say 2.0.7 until re-pinned.
4. Read the changelog for anything that fixes what an extension works around (DESIGN §7 lists the native fixes we
   asked for). Fixed upstream → mark it in DESIGN and in `/ue-blackeye`'s Custom fixes table.

## When the engine updates

Rebuild and reinstall (hub `refs/build-install.md`); re-pin the engine citations in DESIGN (they say 5.8.2).

## Log

- 2026-10-06 (end of session 1): P0 done, P1 built (twin, lock/unlock, menu, undo, DynamicLens copy, settle). Two
  menu crashes fixed. Open alignment issue handed over in `status.md`. Build scripts (all repos + template) now
  treat the crash reporter as a running editor and fail fast on locked files.

- 2026-10-06: repo created as the home for Black Eye extensions; Fast Bake plan moved in as
  `docs/fast-bake/DESIGN.md`; plugin skeleton with the contract self-test.
