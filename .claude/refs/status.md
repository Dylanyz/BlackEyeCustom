# Status and handoff (read first when picking up Fast Bake)

Last updated 2026-10-07. The design and every measured number are in `docs/fast-bake/DESIGN.md`; this file says where
things stand and what to do next.

## Phase

- **P0 (spike, measurements): done.** DESIGN §8, §10.
- **P1 (twin, lock/unlock, menu, undo): built and installed; the twin-tag fix (trap 4.17) installed and verified 2026-10-06.** DESIGN §5, §6.
- **Bake Edit (bake only what an edit shows, with handles): built and installed 2026-10-07**, verified on the repro
  (planner, bake, keep-other-keys, seams). Selected-sections mode added the same day. DESIGN §6.
- **Bake Edit from the master, shot controls, one window for every way in: built, installed and measured on the
  repro 2026-10-07** (DESIGN §6 "Measured on the repro"). Master bake = shot-by-shot bake to 0.0001 deg.
- **Shot list (lock / unlock / delete per shot and all), Delete, "Their whole shots", Content Browser Lock / Unlock /
  Delete Bakes: built and installed 2026-10-07 (12:40 DLL), not run yet; committed as WIP at Dylan's request 2026-10-07, untested.** DESIGN §6 "Shot list, delete, whole shots".
  Next: SelfTest, then on the repro: Unlock all / Lock all in the master with and
  without "Only what's selected", Delete one and Delete all + one undo, whole-shots plan and a master bake of it, the
  Content Browser entries.
- **P2 (batch, stale flag, re-sync settings): not started.** DESIGN §10.

## Open issues

- **Content Browser "Black Eye: Bake..." with several sequences:** not seen yet (synthetic right-clicks don't open
  that menu); Dylan to try by hand.
- **Production master:** a `_BAKE-TEST` copy, with Dylan's say-so: time the first editor frame after a master bake
  (the shots recompile; hypothesis: seconds, not the reopen's minutes).

- The Anim Mixer freeze (DESIGN trap 4.11) may have been the frozen frame counter too (trap 4.18). Re-test in the
  tester project without `-DisablePlugins`.
- Keep the bake matching live Black Eye playback (Dylan, 2026-10-06). Considered, not built: re-snap the camera when a
  Follow target jumps in one frame (DESIGN §10). Epic-side costs we can't fix: DESIGN §9.

## Where things are

- **Test project:** the tester project, repro at `/Game/Claude/FastBake/` (launch with
  `-DisablePlugins=MetaHumanCrowdContent,MovieSceneAnimMixer`, DESIGN trap 4.11).
- **Production checks** run in the film project only with Dylan's say-so; its specifics are in that project's
  `.claude/refs/blackeye-fast-bake.md`, never here. Test duplicates of angle shots carry the suffix `_BAKE-TEST` and
  live in the film's `/Game/Claude/FastBake/Demos/` (Dylan, 2026-10-06); modify only those, never the real shots.
- **Editor scripting from Git Bash:** `MSYS_NO_PATHCONV=1` before `/Game/...` arguments; sequences must be open
  on their own in Sequencer before `bake_shot` (it opens and asks to re-run otherwise).
- **Keep demos short:** Dylan, 2026-10-06: "please dont rebake a whole long 10 minutes. just demo things on shorts
  shots right now, like under 1 minute for the bakes". Use `start_frame` / `end_frame`.

## Roadmap

1. P2: Content Browser batch over a shots folder; stale flag (BEC tracks or subject sections changed since the bake);
   re-sync camera settings without re-baking.
2. Speed (P3, only if needed): shared subject pass across angles (DESIGN §9).
