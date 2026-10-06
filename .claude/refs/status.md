# Status and handoff (read first when picking up Fast Bake)

Last updated 2026-10-06, second session (alignment checked against playback, twin-tag fix built). The design and every measured number are in
`docs/fast-bake/DESIGN.md`; this file says where things stand and what to do next. Keep it current: update it at the
end of every session that changes the plugin.

## Phase

- **P0 (spike, measurements): done.** DESIGN §8, §10.
- **P1 (twin, lock/unlock, menu, undo): built and installed; the twin-tag fix (trap 4.17) is built, not installed.** DESIGN §5, §6.
- **P2 (batch, stale flag, re-sync settings): not started.** DESIGN §10.

## What works (verified in the editor, 2026-10-06)

- `UBlackEyeFastBakeLibrary::BakeShot` / `SetLocked` / `GetBakeInfo` (Python: `unreal.BlackEyeFastBakeLibrary`).
- Sequencer binding right-click > Black Eye Fast Bake > Bake and lock / Lock / Unlock. Two crashes found and fixed
  (DESIGN §6). Console twins: `BlackEyeCustom.FastBake.Bake <binding>`, `BlackEyeCustom.FastBake.MenuTest`.
- Twin gets DynamicLens and the camera's lens setup; twin playback equals the baked samples exactly.
- Undo works for bake, lock, unlock. A locked demo edit played with no cut jolts (DESIGN §5 table).
- Settle after the opening snap (`SettleSeconds`, default 10 s).
- Debug switches: `BlackEyeCustom.FastBake.Debug <bitmask>` (bisecting), `BlackEyeCustom.FastBake.Verbose 1`.

## Alignment issue: findings (second session, 2026-10-06)

Dylan: "the bake seems further away than the blackeye", then "still not lined up". Where it stands:

- **Confirmed: the bake equals live playback.** Same production test shot, live camera parked 10 s on the first frame
  and played unlocked (~70-76 editor fps), sampled every world tick: twin vs live at most 1.1 cm / 0.10 deg; CSV bakes
  at 1-3 sub-steps within 0.4-1.1 cm. DESIGN trap 4.16.
- **Ruled out:** hypothesis 2 (rendered view = component transform to 0.000) and 3 (Black Eye 2.0.7 overrides neither
  `GetCameraView` nor `CalcCamera`). Hypothesis 4 (tick rate) is real but ~1 cm on this shot. 5 not tested.
- **Found and fixed (built, NOT installed; the film's editor was open): DESIGN trap 4.17.** The twin's binding tag
  held five twin IDs, four of them dead, and the code read the first. Effects: re-bakes of a locked shot made new twins
  the cut never showed (the shot kept playing an *older* bake, very likely what Dylan saw "after the last fix");
  Unlock did nothing and the menu offered Lock on a locked shot; a bake with no camera named baked the spare BEC.
- **Still a hypothesis:** what Dylan compares against. A parked or scrubbed live camera has caught up with its
  subject; playback (and the bake) lag by Follow damping (1 s here), so they differ by tens of cm whenever the subject
  moves (4.16). Ask Dylan how he compares (parked toggle, edit playback, render).

**Next:**
1. Install (`Toolsuild_blackeyecustom.ps1 -InstallOnly`, editor closed, Dylan's go), relaunch, `BlackEyeCustom.SelfTest`.
2. On the test shot: `GetBakeInfo` should report the twin, locked. Unlock from the menu should show the live camera;
   Bake and lock should rewrite the same twin and leave the tag with one ID.
3. Re-bake, lock, and have Dylan compare in playback. If he still sees an offset, record the edit's playback with the
   shot unlocked vs locked (the record pattern in DESIGN 4.16) to see if it's the edit context.

## Where things are

- **Test project:** `MDR_58_tester`, repro at `/Game/Claude/FastBake/` (launch with
  `-DisablePlugins=MetaHumanCrowdContent,MovieSceneAnimMixer`, DESIGN trap 4.11).
- **Production checks** run in the film project only with Dylan's say-so; its specifics are in that project's
  `.claude/refs/blackeye-fast-bake.md`, never here. Dylan's short test duplicate of an angle (suffix `_BAKE-TEST`)
  was re-baked with the settle fix and left unsaved at session end.
- **Editor scripting from Git Bash:** `MSYS_NO_PATHCONV=1` before `/Game/...` arguments; sequences must be open
  on their own in Sequencer before `bake_shot` (it opens and asks to re-run otherwise).
- **Keep demos short:** Dylan, 2026-10-06: "please dont rebake a whole long 10 minutes. just demo things on shorts
  shots right now, like under 1 minute for the bakes". Use `start_frame` / `end_frame`.

## Roadmap after the open issue

1. Install and verify the twin-tag fix, then close the alignment issue above with Dylan.
2. Optional: bake the union of the shot's playback range and every range its sections use (Dylan asked whether
   only start/end is baked; it is).
3. P2: Content Browser batch over a shots folder; stale flag (BEC tracks or subject sections changed since the bake);
   re-sync camera settings without re-baking.
4. Speed (P3, only if needed): shared subject pass across angles (DESIGN §9).
