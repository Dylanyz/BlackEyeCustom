# Status and handoff (read first when picking up Fast Bake)

Last updated 2026-10-06, third session (speed and stability audit, installed and verified). The design and every measured number are in
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

## Speed and stability audit: done 2026-10-06 (third session), installed and verified

- **Speed:** each bake step now advances `GFrameCounter` (DESIGN trap 4.18). Sequencer had been re-posing every
  animated mesh on every step. 1-minute range 11-12x → 21.7x realtime; short shot 9.7x → 15.5x. Output unchanged
  (A/B in one build via `BlackEyeCustom.FastBake.Debug 64`). Numbers: DESIGN section 8, "Speed and stability audit".
- **Write:** the twin alone respawns (`DestroySpawnedObject`), not the whole scene; the twin is written at the first
  baked frame, so a playhead parked outside the shot no longer leaves a half-written twin (verified).
- **Stability:** evaluation status fixed at Stopped; a re-spawned camera is deselected (trap 4.2); settle after opening a
  sequence = 0.5 s and 3 frames; the realtime record is stopped on module shutdown.
- Verified in the film project on the `_BAKE-TEST` shot: SelfTest PASSED, BakeCameraToCsv / BakeShot / the console
  Bake from inside the edit (opens the shot, bakes, puts the view back).
- **Not fixed, measured:** Sequencer's editor recompile check, 0.9 ms of every evaluation (Epic-side, DESIGN section
  9). The write's remaining ~0.15 s is the shot's MetaHumans re-initialising when the playhead jumps into the shot.
- **Open:** the Anim Mixer freeze (trap 4.11) may have been the frozen frame counter too. Re-test in the tester
  project without `-DisablePlugins`.

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

1. Optional: bake the union of the shot's playback range and every range its sections use (Dylan asked whether
   only start/end is baked; it is).
2. P2: Content Browser batch over a shots folder; stale flag (BEC tracks or subject sections changed since the bake);
   re-sync camera settings without re-baking.
3. Speed (P3, only if needed): shared subject pass across angles (DESIGN §9).
