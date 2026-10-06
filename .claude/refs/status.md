# Status and handoff (read first when picking up Fast Bake)

Last updated 2026-10-06, end of the first session. The design and every measured number are in
`docs/fast-bake/DESIGN.md`; this file says where things stand and what to do next. Keep it current: update it at the
end of every session that changes the plugin.

## Phase

- **P0 (spike, measurements): done.** DESIGN §8, §10.
- **P1 (twin, lock/unlock, menu, undo): built and installed, with one open issue (below).** DESIGN §5, §6.
- **P2 (batch, stale flag, re-sync settings): not started.** DESIGN §10.

## What works (verified in the editor, 2026-10-06)

- `UBlackEyeFastBakeLibrary::BakeShot` / `SetLocked` / `GetBakeInfo` (Python: `unreal.BlackEyeFastBakeLibrary`).
- Sequencer binding right-click > Black Eye Fast Bake > Bake and lock / Lock / Unlock. Two crashes found and fixed
  (DESIGN §6). Console twins: `BlackEyeCustom.FastBake.Bake <binding>`, `BlackEyeCustom.FastBake.MenuTest`.
- Twin gets DynamicLens and the camera's lens setup; twin playback equals the baked samples exactly.
- Undo works for bake, lock, unlock. A locked demo edit played with no cut jolts (DESIGN §5 table).
- Settle after the opening snap (`SettleSeconds`, default 10 s).
- Debug switches: `BlackEyeCustom.FastBake.Debug <bitmask>` (bisecting), `BlackEyeCustom.FastBake.Verbose 1`.

## OPEN ISSUE: the bake doesn't line up with the live Black Eye camera (Dylan, 2026-10-06)

Dylan: "the bake seems further away than the blackeye" and, after the last fix, "the bake is still not lined up with
the black eye camera, we need to keep working on that." **Treat this as unresolved.** The last agent's explanation (a
parked live camera has caught up with its subject, the bake shows playback's lag) was measured but did not satisfy
Dylan, and it was never checked against *playback*.

What was measured (production shot, a 134-frame duplicate of an angle, subject's pelvis pops 76 cm 4 frames in):
- Lens identical on twin and live camera (focal 30, hFOV 61.75, filmback 23.0 x 18.66, overscan 0.08, crop 1.78).
- Rotation nearly identical; position differs: twin 40-90 cm **behind** the live camera along the view axis.
- Parked live camera's rig root sits on the Follow target (pelvis + Follow offset) to 0.4 cm; the baked rig root
  was ~40 cm off it 34 frames in, ~60 cm before the settle fix.
- At the shot's first frame, twin and parked live camera agree to 13 cm; the gap opens after the subject's pop.

Hypotheses not yet tested, most promising first:
1. **Compare against playback, not a parked camera.** Record the viewport during unlocked playback of the same range
   (the Slate-tick recorder pattern in DESIGN §5's edit test, or `StartRealtimeRecord`) and compare with the twin's
   keys. If playback matches the twin, the issue is only the parked comparison; if not, there is a real bake bug.
2. **The rendered view isn't the camera component's world transform.** `UCameraComponent::GetCameraView` adds
   `AdditiveOffset` / `AdditiveFOVOffset` when `bUseAdditiveOffset` is set (shakes, rigs, camera templates). The bake
   samples `GetComponentToWorld()`. Compare `GetCameraView()` on the live camera with its component transform.
3. **Black Eye's own camera view override.** Check whether `UBlackEyeCineCameraComponent` overrides `GetCameraView`
   or the actor's `CalcCamera` (Black Eye source, `Components/BlackEyeCineCameraComponent.*`).
4. **Tick rate** (trap 4.12): measured 2.6 cm / 1 deg in the tester; may be much larger on a rig with dead zones and
   a pedestal arm. Bake with `sub_steps` matching the editor's ticks per frame (~2.7 in that project) and compare.
5. **Active viewport size** feeds LookAt's projection (it changed between runs: 828x682 vs 828x412).

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

1. Close the alignment issue above (playback comparison first).
2. Optional: bake the union of the shot's playback range and every range its sections use (Dylan asked whether
   only start/end is baked; it is).
3. P2: Content Browser batch over a shots folder; stale flag (BEC tracks or subject sections changed since the bake);
   re-sync camera settings without re-baking.
4. Speed (P3, only if needed): shared subject pass across angles (DESIGN §9).
