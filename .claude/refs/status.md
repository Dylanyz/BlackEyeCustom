# Status and handoff (read first when picking up Fast Bake)

Last updated 2026-10-06, second session (alignment closed, twin-tag fix installed and verified). The design and every measured number are in
`docs/fast-bake/DESIGN.md`; this file says where things stand and what to do next. Keep it current: update it at the
end of every session that changes the plugin.

## Phase

- **P0 (spike, measurements): done.** DESIGN §8, §10.
- **P1 (twin, lock/unlock, menu, undo): built and installed; the twin-tag fix (trap 4.17) installed and verified 2026-10-06.** DESIGN §5, §6.
- **P2 (batch, stale flag, re-sync settings): not started.** DESIGN §10.

## What works (verified in the editor, 2026-10-06)

- `UBlackEyeFastBakeLibrary::BakeShot` / `SetLocked` / `GetBakeInfo` (Python: `unreal.BlackEyeFastBakeLibrary`).
- Sequencer binding right-click > Black Eye Fast Bake > Bake and lock / Lock / Unlock. Two crashes found and fixed
  (DESIGN §6). Console twins: `BlackEyeCustom.FastBake.Bake <binding>`, `BlackEyeCustom.FastBake.MenuTest`.
- Twin gets DynamicLens and the camera's lens setup; twin playback equals the baked samples exactly.
- Undo works for bake, lock, unlock. A locked demo edit played with no cut jolts (DESIGN §5 table).
- Settle after the opening snap (`SettleSeconds`, default 10 s).
- Debug switches: `BlackEyeCustom.FastBake.Debug <bitmask>` (bisecting), `BlackEyeCustom.FastBake.Verbose 1`.

## Alignment issue: closed 2026-10-06

The bake matches live Black Eye playback (0.6 cm on the re-baked test shot); Dylan agreed and wants it to keep
matching playback. What looked like misalignment was (1) re-bakes playing an older twin, fixed (DESIGN trap 4.17,
verified: one twin, one tag ID, locked, bake info filled), and (2) comparing against a live camera while stepping
frames, which catches up between steps, across a 76 cm subject pop at a mocap take change (trap 4.16). Considered,
not built: re-snap the camera when a Follow target jumps in one frame (DESIGN section 10).

## Where things are

- **Test project:** the tester project, repro at `/Game/Claude/FastBake/` (launch with
  `-DisablePlugins=MetaHumanCrowdContent,MovieSceneAnimMixer`, DESIGN trap 4.11).
- **Production checks** run in the film project only with Dylan's say-so; its specifics are in that project's
  `.claude/refs/blackeye-fast-bake.md`, never here. Dylan's short test duplicate of an angle (suffix `_BAKE-TEST`)
  was re-baked with the settle fix and left unsaved at session end.
- **Editor scripting from Git Bash:** `MSYS_NO_PATHCONV=1` before `/Game/...` arguments; sequences must be open
  on their own in Sequencer before `bake_shot` (it opens and asks to re-run otherwise).
- **Keep demos short:** Dylan, 2026-10-06: "please dont rebake a whole long 10 minutes. just demo things on shorts
  shots right now, like under 1 minute for the bakes". Use `start_frame` / `end_frame`.

## Roadmap

1. Optional: bake the union of the shot's playback range and every range its sections use (Dylan asked whether
   only start/end is baked; it is).
2. P2: Content Browser batch over a shots folder; stale flag (BEC tracks or subject sections changed since the bake);
   re-sync camera settings without re-baking.
3. Speed (P3, only if needed): shared subject pass across angles (DESIGN §9).
