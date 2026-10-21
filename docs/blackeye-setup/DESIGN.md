<!-- Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
     SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0 -->
# Black Eye Setup: design

The one living document for Black Eye Setup. Each section carries a status: **planned** → **built** → **measured**.
Rejected ideas stay in, with the reason.

Citations are `file:line`, pinned to **Black Eye 2.0.7** and **Unreal Engine 5.8.3**. No Black Eye or engine code is
reproduced here.

| Section | Status |
|---|---|
| [1. The problem](#1-the-problem) | from production use and source |
| [2. What it does](#2-what-it-does) | built, measured on the repro 2026-10-21 |
| [3. Which camera](#3-which-camera) | built |
| [4. The switches, rule by rule](#4-the-switches-rule-by-rule) | built, measured on the repro 2026-10-21 |
| [4a. From the playhead](#4a-from-the-playhead) | built, measured on the repro 2026-10-21 |
| [5. Tracks](#5-tracks) | built, measured |
| [6. Subjects and fill from character](#6-subjects-and-fill-from-character) | built, measured on the repro |
| [7. Undo](#7-undo) | built, measured |
| [8. Traps](#8-traps) | measured |
| [9. What becomes trivial inside Black Eye](#9-what-becomes-trivial-inside-black-eye) | planned |
| [10. Decisions and rejected ideas](#10-decisions-and-rejected-ideas) | live |
| [11. Measured](#11-measured) | measured |
| [12. Open](#12-open) | live |

---

## 1. The problem

*Status: from production use and source.*

Using a BEC as a normal camera, or switching it between procedural and hand-made, is hard to get right:

- **LookAt off still drives the camera.** With `bEnableLookAt` false the LookAt tick still runs: it aims at a
  virtual target straight ahead at `ManualFocusDistance` with damping 1000 (`BlackEyeLookUtils.cpp:313-335`) and calls
  `SetWorldTransform` on the camera component (`BlackEyeLookAtComponent.cpp:522`). Rotation keyed or set by hand on
  the camera component doesn't stick; the aim is integrator state.
- **Black Eye's focus locks at 1.5 cm with LookAt off**: one frame with the focus target behind the lens writes
  focus ≤ 0, the CineCamera floors it at 1.5 cm, and the Plate/Pedestal guard (`BlackEyeLookUtils.cpp:484-503`) then
  skips the focus block every tick while `BlackEyeLookAtComponent.cpp:529` writes the old value back.
- **Every switch needs its own Sequencer track setup.** UE auto-adds a Transform track to a spawnable camera, and its
  unkeyed channels still apply their defaults every frame, fighting Follow and LookAt (Black Eye manual, "Camera
  jitters with Black Eye cameras in Sequencer"). A spawnable camera's subject references are lost in PIE and
  renders unless keyed (manual, "Spawnable Black Eye Cameras in Sequencer").
- **Turning Follow on in Details overwrites Follow Offset** (`BlackEyeFollowComponent.cpp:308-321`), but a keyed
  toggle skips that, so the same switch behaves two ways.

## 2. What it does

*Status: built, measured on the repro 2026-10-21.*

Sequencer toolbar → **Black Eye Setup** (beside Bake Edit) opens a small window for one BEC of the focused sequence:

- **Three switches**, each independent: **Follow**, **LookAt**, **Focus**. Each one sets the camera *and* its tracks
  so the camera keeps its place when switched and nothing fights it.
- **A subject per switch**, with **Use Selected** (the actor selected in the level).
- **Fill all from selected character**: Follow its `Heading` socket (else pelvis), LookAt and Focus its tracker.
- **From the playhead on (keyed)**: switches and subjects change from the playhead on only (§4a); off, the whole shot.
- A status line saying what each action changed, and warnings (keyed switches, weight 0).

| Switch | On | Off |
|---|---|---|
| Follow | follows its subject; Follow Offset measured from where the camera is | camera stays put; position is the Transform track's |
| LookAt | aims at its subject | aim stays where it is and becomes the Transform track's rotation |
| Focus | lens Tracking focus on the subject, every frame | manual focus, held at the current distance |

Also: `UBlackEyeSetupLibrary` (`GetSetupState`, `SetSetupSwitch`, `SetSetupSubject`, `AutofillSetup`) and console
commands `BlackEyeCustom.Setup.Open | State | Switch <Follow|LookAt|Focus> <0|1> [Camera] | SwitchHere <Follow|LookAt> <0|1>
[Camera] | Fill [Camera]`. Every action
is one undo step.

## 3. Which camera

*Status: built.*

In the focused sequence, the first that applies (`FindCamera`, `BlackEyeSetup.cpp`): the name given (the window's
picker); a selected BEC binding or one of its component bindings; the BEC the camera cut under the playhead plays,
then any cut's; the first BEC binding. A spawnable must be spawned at the playhead (its live copy is read for the
current pose); the window says so otherwise.

## 4. The switches, rule by rule

*Status: built, measured on the repro 2026-10-21 (§11).*

Writes go to the **template** (with `Modify`, for undo) and to the **spawned copy** (without `Modify`), so the view
changes now and the shot keeps it. The copy gets no `Modify` because the editor spawn register saves a modified copy
over its template on destroy or save (`LevelSequenceEditorSpawnRegister.cpp:106-125`, `:236-282`), Black Eye's
per-tick writes included: that is how a 1.5 cm focus got saved into templates in production. A level (possessable)
camera is one object, edited with `Modify`.

**Follow on.** Needs a subject. Follow Offset is measured from the camera's current mount position in the frame
Follow will use, built as Black Eye builds it for one subject (`BlackEyeFollowComponent.cpp:141-233`: the bone or
component pose, Orientation Reference Mode, then Pitch/Yaw Offset), so the camera doesn't move when Follow takes
over. Several subjects or chase mode: the offset is kept (not modelled). If LookAt is off, Final Orientation Mode is
set to Manual so Follow moves the camera without turning it (the rotation stays the user's).
BE-NATIVE: `GetTargetTransform` exposed, or the Details path's re-solve made an explicit, keyable-safe action.

**Follow off.** `bEnableFollow` false. Follow returns early (`BlackEyeFollowComponentBase.cpp:160-164`), so the root
stays where it is; the Transform track's Location takes over with the current location as its value. The Follow
subject's weight goes to 0 (back to 1 when Follow comes on): a freshly spawned camera snaps Follow once even while it is
off, because the snap flag set in the constructor (`BlackEyeFollowComponentBase.cpp:44`) bypasses `bEnableFollow`
(`:160-164`). Measured: with Follow off, a respawned camera sat at its Follow position (266, -723) until a scrub put it
back on its track (-400, -400); with weight 0 the subject is invalid (`BlackEyeWeightedTarget.h:33-35`), the snap has
nothing to move to, and the respawned camera stayed on its track. The subject itself is kept.
BE-NATIVE: skip the spawn snap while Follow is off.

**LookAt on.** Needs a subject; a subject weight of 0 is set to 1 (weight 0 makes Black Eye ignore the subject,
`BlackEyeWeightedTarget.h:33-35`, which silently froze aim and focus in production).

**LookAt off.** The lens's current world rotation goes onto the root, and the lens's own relative rotation to zero,
so the view doesn't move and the root (Transform track Rotation) aims the camera from now on. Black Eye still
rebuilds the lens from the root every tick (`BlackEyeLookAtComponent.cpp:522`, `BlackEyeLookUtils.cpp:635-657`): with
LookAt off it aims along the lens's own forward, which now rides with the root. If Follow is on, Final Orientation
Mode becomes Manual. `bSetFocusDistance` is turned off (see Focus).
BE-NATIVE: with LookAt off, leave the lens to the actor (`BlackEyeLookAtComponent.cpp:522`).

**Focus on.** The lens's own Tracking focus (`FocusSettings.FocusMethod = Tracking`, `ActorToTrack` = subject),
worked out by the engine every frame with no state (`CineCameraComponent.cpp:556-582`), never Black Eye's. Black
Eye's `bSetFocusDistance` goes off so it never writes focus. `ManualFocusDistance` is set to 100000: with LookAt off
Black Eye aims at a virtual point that far ahead (`BlackEyeLookUtils.cpp:313-335`), and far keeps that aim steady and
the Plate/Pedestal guard away (skill ref `lookat-off-focus.md` §4A). The lens tracks actors, not bones: a bone subject
becomes a fixed offset from its actor as it is now (a tracker is the way to follow a bone).
BE-NATIVE: run the focus block whether or not pointing runs (`BlackEyeLookUtils.cpp:611-620`).

**Focus off.** Manual focus at the distance the lens has now. The lens works out `CurrentFocusDistance` only when
something views it (`CineCameraComponent.cpp:595`), so the setup resolves it first (`GetCameraView`, as Fast Bake does,
trap 4.19 there); without that it read 0 and held 0 (measured 2026-10-21).

## 4a. From the playhead

*Status: built, measured on the repro 2026-10-21 (§11).*

The window's **From the playhead on (keyed)** box (library `bFromPlayhead`, console `BlackEyeCustom.Setup.SwitchHere`):

- **Follow / LookAt:** the switch is keyed at the playhead (an unkeyed track first keeps the old value from the playback
  start). Follow also keys its subject weight (§4) and, when on, its measured Follow Offset, with constant keys. The
  Transform section is split at the playhead (`UMovieSceneSection::SplitSection`) and only the part from the playhead on
  changes its channels, since channel masks are per section. The setup itself keeps the shot's start.
- **A switch that is already keyed is always switched at the playhead**, whatever the box says: a default can't beat
  its keys. The window shows a keyed switch's value at the playhead (the live copy, which Sequencer evaluated).
- **Subjects:** the subject reference is keyed at the playhead; the subject before it stays. Only the actor can be keyed:
  Component Name and Bone Name are plain strings (`BlackEyeSimpleTarget.h:50`, `:59`), so the new subject must use the
  same component and bone (tracker to tracker; the same socket on the same rig), else it is refused with that reason.
- **Focus on/off is whole-shot only:** the lens's `FocusMethod` isn't `Interp` (`CineCameraSettings.h:239`), so it
  can't be keyed. Focus *subject* changes from the playhead work (`ActorToTrack` is keyable, `:215`).
- Follow on from the playhead: Black Eye treats the subject becoming valid as a new target and starts from where the
  camera is (`BlackEyeFollowComponent.cpp` "so the camera doesn't jump", in TickFollow), and the offset was measured
  from there, so the switch doesn't jump (measured).

## 5. Tracks

*Status: built, measured.*

- **Transform track: channels, not the whole track.** Location is active only while Follow is off; Rotation only while
  LookAt is off and Follow leaves rotation alone (off, or Manual orientation). Scale is untouched. Set with the
  section's channel mask (`UMovieScene3DTransformSection::SetMask`); the UE manual: unticked "Active Channels" are not
  evaluated. Channels switched on with no keys take the camera's current place as their value, so the camera doesn't
  jump to the track's old defaults. Keys are never touched: masked keys come back when the channel does. A section an
  older fix had deactivated is re-activated (its channels now say what it drives). A spawnable with no Transform track
  gets one: a movie render of a spawnable cut camera without Spawn or Transform track renders a fallback view.
  BE-NATIVE: Black Eye would ignore the channels its switches drive (manual, "Camera jitters").
- **Switch and offset tracks.** An unkeyed `bEnableFollow` / `bEnableLookAt` / `FollowOffset` track takes the new value
  as its default; a keyed one is left alone and reported (its keys decide during playback).
- **Reference tracks (spawnable cameras only).** Per subject, an Actor Reference track on the component binding, its
  default (and any old keys) pointing at the subject's binding: `Follow` `Target_0.Actor`, `LookAt` `Target_0.Actor`,
  `CameraComponent` `FocusSettings.TrackingFocusSettings.ActorToTrack`. A subject with no binding in the sequence is
  bound (a possessable with no tracks). A subject spawned by another sequence can't be referenced; reported. Measured:
  with all three references cleared on the spawned copy, one evaluation put them all back (§11).

## 6. Subjects and fill from character

*Status: built, measured on the repro (pelvis path, tracker attached to the head).*

- **Follow:** the character's skeletal mesh with a `Heading` socket (X = facing, Z = up), else its pelvis bone
  (`pelvis`, `Bip01-Pelvis`, `hips`, ...; a MetaHuman's `Body` is searched first). The component is named explicitly:
  an empty Component Name resolves to the actor's root component (`BlackEyeSimpleTarget.h` `GetTargetComponent`).
  Subject Heading takes the bone's X axis flattened (`BlackEyeFollowComponent.cpp:191`), and pelvis X points up the
  spine on most rigs, hence the socket.
- **LookAt / Focus:** the character's tracker: an actor attached to it that is a TargetPoint or labelled `tracker*`,
  else such an actor within 60 cm of its head bone. No tracker: LookAt the head bone, Focus the actor (warned).
- **Use Selected** applies the same rules to one switch; a selected tracker is used as itself.

## 7. Undo

*Status: built, measured.*

Each action is one transaction. Undo restores the template and tracks, but the spawned copy (edited without
`Modify`, §4) kept the undone state, even after scrubbing (measured 2026-10-21). An undo client (titles starting
"Black Eye Setup") respawns every spawned BEC of the open sequences after undo / redo, as Sequencer does after a
template change (`SequencerUtilities.cpp:5657-5661`). The respawned camera starts with a snap
(`BlackEyeFollowComponentBase.cpp:44`). Measured: after undo the root rotation returned to the template's.

## 8. Traps

*Status: measured.*

1. **A BEC only moves while the editor ticks the world.** With the Sequencer window maximised over the level viewport,
   or the editor in the background with "Use Less CPU when in Background" on, nothing ticked: the camera sat at its
   spawn transform through every test step (2026-10-21). Test with a visible viewport and that setting off.
2. **`CurrentFocusDistance` is 0 until the lens is viewed** (§4, Focus off).
3. **Quitting the editor with a sequence open in Sequencer crashed in shutdown** (access violation in
   PropertyEditor under Sequencer, twice, before and with this extension loaded); closing the sequence first
   (`LevelSequenceEditorBlueprintLibrary.close_level_sequence`) quit cleanly.
4. **A Black Eye camera snaps Follow once on spawn even with Follow off** (§4, Follow off).
5. **The window must not hold actor pointers.** It kept the state struct with its subject actors between refreshes;
   a level change destroyed them and the next paint crashed in `Describe` (2026-10-21). It now keeps only text.
6. **Synthetic (computer-use) clicks did not reach the window's buttons** (or any Slate widget that session; hover
   tooltips showed). The actions were tested through the library and console commands, which the buttons call.

## 9. What becomes trivial inside Black Eye

*Status: planned.* Each `BE-NATIVE` comment in `Source/.../Private/BlackEyeSetup/` is the port checklist:

- LookAt off leaves the lens alone; the focus block runs without pointing. Most of §4 then disappears.
- The camera knows its switches: it can mask its own Transform channels and key its own references.
- Follow's offset re-solve as an explicit action, the same from Details and Sequencer.
- A 0-1 influence per switch, so switching can blend instead of pop.

## 10. Decisions and rejected ideas

- **Edit template + live copy, not Save Default State.** Saving the live copy also saves Black Eye's per-tick
  writes (the 1.5 cm focus). Rejected: copying the whole live object.
- **The lens's Tracking focus, not Black Eye's.** Black Eye's needs LookAt running and can lock; the lens's has no
  state. Cost: no bone targets (trackers instead).
- **Channel masks, not muting the Transform track.** Muting loses hand keys for the channels the user still needs
  (position with Follow off, aim with LookAt off). Masks keep keys and the track (renders want it).
- **Whole shot by default, keyed from the playhead on request** (§4a). Whole-shot edits defaults and leaves keys alone;
  the playhead mode keys and splits. Rejected: always keying (a shot set up once would fill with keys).
- **Separate extension from Fast Bake**, same module (Dylan, 2026-10-20: "another module beside fastbake, this one
  focused on blackeyeSetup").

## 11. Measured

Repro: Manny walking a bent path (`Tools/fast_bake_repro.py`), spawnable BEC, a TargetPoint `tracker_manny` attached to
Manny's head; MDR_58_tester, UE 5.8.3, Black Eye 2.0.7, 2026-10-21. Camera lens world pose read before, right after and
2 s after each action (the editor ticking).

| Action | Result |
|---|---|
| Focus on | lens focus = the tracker's depth along the lens axis (976.92 = 976.92 cm) |
| Focus off | held at the tracked distance (979.77 cm) |
| LookAt off (Follow on) | aim unchanged; lens settled back to the same spot after one tick; Follow then moved the camera with Manny, rotation constant |
| Follow off | no move; camera fixed while Manny walked on |
| Follow on (LookAt off) | no jump at the switch; then followed Manny |
| LookAt on | re-aimed at the tracker with its damping (31.9° → 36.4° yaw) |
| Fill from Manny | Follow → `SkeletalMeshComponent0 > pelvis` (no Heading socket), LookAt / Focus → tracker; switches unchanged |
| References | all three cleared on the spawned copy, restored by one evaluation |
| Undo of LookAt off | template restored; spawned copy respawned (root rotation back to the template's) |
| Follow off, then respawn | before the weight fix: snapped to its Follow spot (266, -723) until a scrub; after: stayed on its track (-400, -400) |
| Follow off from f150 (keyed) | f120 still followed Manny; f190 held exactly the f150 spot (266.17, -722.82) |
| LookAt off from f200 (keyed) | f230 aim held at 60.83 deg; f170 LookAt still aiming |
| Follow on from f250 (keyed) | no jump at f250; f280 followed Manny, aim unchanged |
| LookAt subject to tracker_b from f100 | f80 aimed at Manny's tracker (0.00 deg off), f150 at tracker_b (0.00 deg off) |
| Window open across two level loads | no crash (after the fix in §8.5) |
| Movie render (6 frames, all on; and LookAt off) | subjects kept in the render world: Manny framed in every frame |
| Self-test | 20 checks ok (7 new) |

## 12. Open

- Click test of the window by hand (synthetic clicks didn't reach it, §8.4).
- Movie render of a shot with keyed switches (measured in the editor only).
- Focus on/off mid-shot: needs a keyable focus method; a workaround would key `ManualFocusDistance` and switch the
  lens's Tracking subject, not done.
- Possessable (level) camera and Heading-socket fill paths: measured once each (fill, LookAt off, Focus on; no
  reference tracks for a level camera).
