<!-- SPDX-License-Identifier: Apache-2.0 -->
# Fast Bake: design

The one living document for Fast Bake: the plan now, the as-built record later. Each section carries a status:
**planned** → **built** → **measured**. Rejected ideas stay in, with the reason.

Citations are `file:line`, pinned to **Black Eye 2.0.7** and **Unreal Engine 5.8.2**. No Black Eye or engine code is
reproduced here; the files cited ship with each product's source.

| Section | Status |
|---|---|
| [1. The problem and its root cause](#1-the-problem-and-its-root-cause) | confirmed from source |
| [2. What already exists, and why none of it fits](#2-what-already-exists-and-why-none-of-it-fits) | confirmed from source |
| [3. The algorithm](#3-the-algorithm) | built |
| [4. Traps](#4-traps) | 4.11-4.17 measured; 4.1 and 4.4 confirmed; the rest from source and review |
| [5. Output: the baked twin, lock and unlock](#5-output-the-baked-twin-lock-and-unlock) | built, measured on a production edit |
| [6. Entry points](#6-entry-points) | library and Sequencer menu built; Content Browser batch is P2 |
| [7. What becomes trivial inside Black Eye](#7-what-becomes-trivial-inside-black-eye) | planned |
| [8. Measured numbers](#8-measured-numbers) | measured on the repro and on a production angle (P0) |
| [9. Decisions and rejected ideas](#9-decisions-and-rejected-ideas) | live |
| [10. Phases](#10-phases) | P0 done; P1 built, alignment checked against playback, twin-tag fix awaiting install (`.claude/refs/status.md`); P2 next |
| [11. Open questions](#11-open-questions) | live |
| [12. Verification](#12-verification) | planned |

---

## 1. The problem and its root cause

*Status: confirmed from source.*

A common cinematic layout: each camera angle is its own shot Level Sequence holding the whole scene, with one
spawnable Black Eye camera (BEC) doing LookAt and Follow. Played as a full shot, the damping looks right. Played
through an edit that cuts between angles, each camera starts offset and jolts into frame. Rendering is fine with
enough warm-up frames, but the film can't be watched in the editor.

**Why it jolts.** The damping state reflects wall-clock editor ticks, not the playhead.

- BEC damping runs in `ABlackEyeCineCameraActorBase::Tick(DeltaTime)` → `Follow->TickFollow` →
  `LookAt->TickLookAt` (`BlackEyeCineCameraActorBase.cpp:124-150`). The step is the world delta; Sequencer time is
  never read.
- In the editor it ticks through `ACineCameraActor::ShouldTickIfViewportsOnly` (`CineCameraActor.cpp:35`), on wall
  clock.
- The damping is `ExponentialSmoothingApprox` (`BlackEyeMath.cpp:61-70`). Its state is the camera's current
  transform and FoV, so there are no velocities to reset.

**Why the camera-cut snap doesn't save it.** Black Eye 2.0's answer to warm-up is `NotifyCameraCut()`, which sets a
one-tick snap flag on Follow and LookAt (`BlackEyeCineCameraActorBase.cpp:68-74`). It doesn't reliably fire in the
editor:

- Game and Movie Render Graph (`MovieSceneCameraCutGameHandler.cpp:333-344`) fire it on every straight or jump cut.
- The editor handler (`MovieSceneCameraCutEditorHandler.cpp:279-352`) fires it only on a jump, or when the locked
  camera *actor changes*. Sequencer scrubs evaluate with `bHasJumped=false` (`Sequencer.cpp:3756`).
- Within a shot, and on cuts back to the same camera, it never fires.
- The snap lasts one tick. If spawnable subjects aren't resolved yet on that tick, the camera damps from wherever
  it was.

**The goal.** An offline, faster-than-realtime bake of each BEC's solved motion into keys, made per shot. A shot can
be **locked** (plays and renders the bake, identical every time) or **unlocked** (live BEC, for tweaking), then
re-baked. The realtime alternative (Take Recorder through `LinkedCamera`, §2) costs the shot's running time per
angle: about 30 minutes per angle, times ten angles a scene. That is the number to beat.

## 2. What already exists, and why none of it fits

*Status: confirmed from source.*

- **No camera "fast bake" exists in UE 5.8.** Grepping the engine for FastBake or SmartBake finds nothing.
- **AutoBake / `FSequencerBaker`** (`MovieSceneTools/Private/Baker/SequencerBaker.cpp`, new in 5.8):
  - Offline and fixed-dt (`1/DisplayRate`), with warm-up frames.
  - Only a skeletal-mesh recorder exists (`FAnimSequenceBakeRecorder`); line 418 reads `//actors to tick? todo`.
  - Its `TickFrameInternal` (383-441) is the per-frame recipe Fast Bake follows:
    1. `EvaluateSynchronousBlocking(...SetHasJumped(true))`
    2. `EvaluateAllConstraints`
    3. per skeletal mesh: `TickAnimation(dt)` → `RefreshBoneTransforms` → `RefreshFollowerComponents` →
       `UpdateComponentToWorld` → `FinalizeBoneTransform`
  - Its recorder interface `ISequencerBakeRecorder` is public (`Public/Baker/ISequencerBaker.h:68`). Fast Bake does
    not plug into it (§9).
- **Sequencer Bake Transform** (`SequencerUtilities.cpp:5103-5360`) only evaluates tracks: it never ticks the actor,
  so it misses BEC damping, and it keys location, rotation and scale only, no focal length. Bake to Control Rig and
  `SequencerTools` share both limits.
- **Black Eye 2.0.7 has no Bake button.** What remains is `LinkedCamera` (`BlackEyeCineCameraActorBase.h:47-50`,
  `.cpp:214-236`): every tick it copies the camera component's world transform, filmback, lens, focus, crop, post
  process and focal / focus / aperture onto a plain CineCamera, which you then Take-Record. Realtime only.
- **Python can't step a BEC.** `Tick`, `TickFollow` and `TickLookAt` aren't UFUNCTIONs, editor ticks use wall-clock
  dt, and `FApp::SetUseFixedTimeStep` isn't exposed. The bake has to be C++.

## 3. The algorithm

*Status: built. The loop is `RunBake` in `Private/FastBake/BlackEyeFastBake.cpp`; step 5 is
`BlackEyeFastBakeTwin.cpp`. With no camera named, the bake takes the Black Eye camera the shot's camera cuts use
(shots often hold a spare one), then the first one bound.*

Per angle-shot Level Sequence, per BEC binding:

1. **Open** the shot in Sequencer as root, so spawnables spawn in the editor world. Save the playhead.
2. **Isolate the camera.**
   - Disable the BEC's actor tick for the duration, so only the bake's steps advance it.
   - Force a constrained aspect ratio (LookAt otherwise reads the viewport's aspect).
   - Disable the perspective viewport's camera-cut handling: `ISequencer::SetPerspectiveViewportCameraCutEnabled(false)`
     (`ISequencer.h:549`). Without it every frame snaps (trap 4.1).
   - Deselect BECs (trap 4.2).
3. **Warm up.** At range start minus N frames (default 0 for a full-range bake), evaluate, then call
   `SnapComponentsToTargetsNow` once, explicitly.
4. **Step every frame** `f` in range, with `dt = 1/DisplayRate`, in `SubSteps` equal steps that land on `f`. The
   default is 1: one camera tick per frame, as a render at Temporal Sample Count 1 gets (trap 4.12).
   1. Evaluate with HasJumped (at the sub-frame time), then constraints.
   2. Refresh only the **subject** skeletal meshes: those of each subject actor and of every actor up its attach
      chain (trap 4.14), parents first (trap 4.6). Re-place socket-attached children. Evaluate constraints again.
   3. `BEC->Tick(dt)`: the virtual `AActor::Tick`, whose BEC override runs Follow then LookAt. Then the camera
      actor's own ticking components, in tick-group order, as a world tick would run them (trap 4.13).
   4. Sample the camera **component** world transform, `CurrentFocalLength`, `FocusSettings.ManualFocusDistance` and
      `CurrentAperture` into arrays.
5. **Write keys** onto the baked twin (§5), in one transaction (trap 4.8).
6. **Restore** the tick, camera-cut handling and playhead; `ForceEvaluate`.
7. Run in chunks under a modal, cancellable `FScopedSlowTask` with a progress bar; GC between chunks.

**What must be captured, and why.**
- The camera *component* world transform, not the actor's: LookAt moves the mount and camera components, not just
  the actor root.
- `CurrentFocalLength`: Dynamic FoV writes it through `SetFieldOfView`.
- `ManualFocusDistance`: `bSetFocusDistance` is on by default.
- Aperture is static, but copy it.

**Black Eye is reached without linking it** (`Source/BlackEyeCustomEditor/Public/BlackEyeContract.h`): BECs are
detected by class path, the snap is called as a UFUNCTION, properties are read through reflection, and stepping
needs no symbol because `Tick` is virtual on `AActor`. Every name used is checked by `BlackEyeCustom.SelfTest`.

## 4. Traps

*Status: from source and an independent review (2026-10-06); 4.1 re-verified in source. Unmeasured until P0.*

1. **Evaluating with HasJumped snaps every frame.** `SetHasJumped(true)` makes the cut a jump cut
   (`MovieSceneCameraCutTrackInstance.cpp:268-271`); the editor handler then calls `NotifyCameraCut`
   (`MovieSceneCameraCutEditorHandler.cpp:279`), which snaps the BEC every frame, so the bake has no damping. Copying
   `FSequencerBaker` as-is inherits this. Fix: disable perspective-viewport camera cuts for the bake (§3 step 2),
   which also skips the handler's `SimulateAllTransforms` cost, and snap once at the start.
2. **Selected BECs crash.** The `IsSelected()` block dereferences `GEditor->GetActiveViewport()` without a null
   check. Deselect first.
3. **Never use `BlackEye.SuspendCameraTick`.** The check sits inside `Tick` itself, so it blocks the bake's own call.
4. **A live editor viewport is required.** LookAt does nothing without a valid viewport, even with Constrain Aspect
   Ratio on (`BlackEyeLookUtils.cpp:392-435`, `LookAtComponent.cpp:487-491`), so a headless or commandlet batch is
   impossible. Assert that frame 1 moved the camera.
5. **Keep the slow task modal.** A mouse button held in the viewport changes LookAt.
6. **Subject refresh order.** Body before Face: the MetaHuman Face copies the head from the Body. Disable URO on
   subjects for the bake.
7. **Likely cost trap (unconfirmed).** `GFrameCounter` doesn't advance inside the loop, so the Sequencer skeletal
   animation system may refresh *every* animated mesh on each evaluation
   (`MovieSceneSkeletalAnimationSystem.cpp:711-721`). Profile in P0.
8. **Undo.** Keep the evaluation loop outside the transaction: sample into arrays, then open one transaction just to
   write keys. Re-resolve subjects every frame, because spawnables re-spawn at section boundaries.
9. **Keys.**
   - Write in bulk with `AddKeys`, not per-frame `AddKey`.
   - Quaternion → Rotator, unwound with `SetClosestToMe`.
   - Auto/cubic interpolation, plus an optional tight-tolerance key reduction (a 30-minute angle is ~43k keys per
     channel otherwise).
   - Twin camera component at identity, scale 1.
   - Copy Filmback, LensSettings clamps, Crop and Overscan exactly; `FocusMethod=Manual`.
10. **Velocity look-ahead is Play-only** (enabled in `BeginPlay`), so an editor bake matches editor playback, not
    PIE.
11. **Measured: Sequencer's experimental Anim Mixer freezes subjects in the bake.** When the `MovieSceneAnimMixer`
    plugin is loaded (pulled in as a dependency, for example by `MetaHumanCrowdContent`), skeletal animation sections
    drive a `SequencerMixedAnimInstance`. The manual refresh in step 4.2 then returns the same pose every frame (the
    head bone's z constant to 0.01 cm over 300 frames), while live playback animates. The legacy path
    (`AnimSequencerInstance`) animates correctly. The bake would aim at a frozen subject, and the "camera moved" check
    can't catch it. P0 ran with `-DisablePlugins=MetaHumanCrowdContent,MovieSceneAnimMixer`. P1: find how the mixer's
    task gets its pose (`MovieSceneAnimInstanceTargetSystem.cpp:143-232` and 480-560), or refuse to bake while a
    subject uses the mixer. `BlackEyeCustom.FastBake.Verbose 1` logs each mesh's anim instance.
12. **Measured: a Black Eye camera's result depends on its tick rate, not only on time.** The same shot baked at 1, 2,
    4 and 8 camera ticks per frame gives four different tracks. At 4 ticks per frame (the editor viewport's ~120 fps
    on a 30 fps sequence) the bake matches realtime playback within realtime's own run-to-run noise. At 1 tick per
    frame it differs by up to 2.6 cm and 1.06 degrees on a slow turn (section 8). `ExponentialSmoothingApprox` is
    step-size independent to about 1e-4 per frame, so the damping curve isn't the cause. Hypothesis (from source, not
    isolated): the LookAt step damps toward a point projected onto an "outer sphere" around the current aim
    (`BlackEyeLookUtils.cpp:520-556`), which depends on the current rotation, so fewer, larger ticks settle elsewhere.
    Consequences:
    - Editor playback, a render and the bake can frame a shot slightly differently, from frame rate alone.
    - A render ticks once per temporal sample (inferred, not measured here), so `SubSteps` should equal the render's
      Temporal Sample Count to match it. Default 1.
    - BE-NATIVE: a frame-rate-independent LookAt/Follow step (or internal sub-stepping at a fixed rate) would make
      editor, render and bake agree.
13. **Measured: the camera's other components must tick too.** On a production camera carrying a lens component
    (DynamicLens), stepping only the actor collapsed the focal length from 30 mm to 0 within ~150 frames, driving the
    field of view toward 180 degrees, and the screen-space composition then aimed about 90 degrees off the subject.
    Cause: Black Eye's known overscan feedback (LookAt shrinks focal by the overscan factor on each tick,
    `BlackEyeLookUtils.cpp` `FBlackEyeLookAtState::UpdateFrom` ~L353), which DynamicLens's guard undoes after Black
    Eye in every world tick. Fix: after `Tick`, run the camera actor's tick-enabled components in tick-group order
    (`TickCameraComponents`). Control: with that switched off (`BlackEyeCustom.FastBake.Debug 32`) the collapse
    returns. Black Eye's own components don't tick (`bCanEverTick` false), so nothing is stepped twice.
14. **Measured: subjects are often trackers attached to a character's bone.** A LookAt subject was a `TargetPoint`
    attached to a MetaHuman Face's `FACIAL_L_Eye` socket; its own actor has no skeletal mesh. The meshes to pose are
    those of every actor up the subject's attach chain, and socket-attached children must be re-placed after the pose
    (`UpdateChildTransforms(OnlyUpdateIfUsingSocket)`, as the engine does for follower meshes,
    `SkinnedMeshComponent.cpp:3348`). With both, the tracker matched live playback to 0.01 cm.
15. **Measured: the opening snap doesn't fully reset Black Eye.** `SnapComponentsToTargetsNow` is a LookAt tick with a
    huge dt (`BlackEyeLookAtComponent.cpp` ~L165), so its result depends on the camera's state before it. Two bakes
    of the same range started from different leftover states agree to 0.05 cm / 0.02 deg mean (max 0.4 cm / 0.1
    deg). A few seconds of warm-up removes even that; renders have the same dependence on their warm-up.
    BE-NATIVE: a snap that solves from a canonical state would make bakes bit-identical.
    **Built (2026-10-06): settle after the snap.** After the opening snap the camera ticks `SettleSeconds` (default
    10 s) with time held at the first frame, as the live camera does while the editor is parked there, so a bake starts
    where the parked live camera sits. Measured on a short production shot: the raw snap left the twin ~60 cm off the
    Follow target a second in; with settling the first frame is within 13 cm of the parked live camera.
16. **Comparing a twin with a parked live camera is not the test.** Parked, a live Black Eye camera keeps ticking until
    it has fully caught up with its subject; during playback it lags by its damping. Measured: a subject whose pelvis
    popped 76 cm four frames into a shot (an animation discontinuity) left the twin gliding ~70 cm over about a second
    (Follow damping 1 s), while the parked live camera sat on the new target, 40-55 cm "closer". The bake is right; the
    reference is playback or a render. Pops in the subject show up as camera glides in both.
    **Measured against playback (2026-10-06):** the same 134-frame production shot, live camera parked 10 s on the
    first frame and then played unlocked at about 70-76 editor fps (~2.4 ticks per frame), sampled after every world
    tick. Twin vs live camera, same tick: at most 1.1 cm / 0.10 deg (the glide after the pop), 0.0-0.1 cm elsewhere.
    CSV bakes vs that record: 1 sub-step 0.34 / 1.14 cm mean / max, 0.05 / 0.11 deg; 2 sub-steps 0.57 cm max; 3 sub-steps
    0.38 cm max (trap 4.12, small on this shot). The rendered view (`UCameraComponent::GetCameraView`) equals the
    component's world transform to 0.000 at every tick, and Black Eye 2.0.7 overrides neither `GetCameraView` nor
    `CalcCamera`, so sampling the component is the right thing to bake.
17. **Measured: a twin's binding tag can name dead twins.** `UMovieScene::TagBinding` appends to the tag's ID list
    (`MovieScene.cpp:593-599`), and a binding removed any way other than Sequencer's Delete (which untags,
    `ObjectBindingModel.cpp:1135-1149`) leaves its ID in the list. A production shot carried five twin IDs, only the
    last one alive. Reading the first ID had three effects:
    - every re-bake created a new twin, and Bake and lock couldn't move the camera cut onto it, because the cut pointed
      at the previous twin rather than the Black Eye camera, so **the shot kept playing an older bake** (the log shows
      every re-bake after the first ending without "locked");
    - Lock / Unlock and the bake info acted on the dead twin: the menu offered Lock on a locked shot, and Unlock changed
      nothing;
    - a bake with no camera named, on a locked shot, didn't recognise the twin in the cut and fell back to the first
      Black Eye camera bound: the shot's unused spare (no subjects, focal 12 mm).
    Fix (built 2026-10-06, `FindTwins`): the twin is the newest tagged ID whose binding exists, the rest are stale. A
    bake retags so the tag names only its twin and moves cuts off stale twins; lock and unlock move cuts on stale twins
    too; with no camera named, a cut on a twin maps back to its Black Eye camera (bake and realtime record alike).

## 5. Output: the baked twin, lock and unlock

*Status: built and measured (2026-10-06).*

**The twin.** A spawnable plain `ACineCameraActor` beside the BEC in the same shot, named `<BEC binding>_Bake`, found
again by a binding tag (`BlackEyeFastBake_<BEC guid>`).
- **Changed from the plan:** a re-bake rewrites the same twin binding rather than deleting and recreating it. In 5.8
  a spawnable is a custom binding (`UMovieSceneSpawnableActorBinding`), so recreating means more machinery for the
  same result, and a locked camera cut keeps pointing at the twin across re-bakes.
- Made with `FSequencerUtilities::MakeNewSpawnable` (Sequencer's Add > Actor path). Not `CreateCamera`: it also locks
  the viewport and adds a camera cut section (`SequencerUtilities.cpp` `NewCameraAdded`).
- Settings: every editable property `UCameraComponent` and `UCineCameraComponent` declare, copied from the BEC's
  **spawnable template** (the authored setup; the spawned instance is transient and carries runtime edits), plus the
  components a user or Blueprint added (DynamicLens). Focus method forced to Manual. Camera component at identity.
- **Measured trap:** a component added to a spawnable template by hand does not survive spawning. The twin is edited
  as its spawned instance and saved with `FMovieSceneSpawnRegister::SaveDefaultSpawnableState` (Sequencer's own
  "Save Default State"); then it does. **Also:** DynamicLens's helper adds its component with `AddInstanceComponent`
  but leaves `CreationMethod` at Native, so components are taken from the instance list too.
- DynamicLens on the twin is safe: its focal writes (kit snap, locked focal, clamp to the measured range) are
  idempotent, and its overscan guard has no feedback to undo on a plain CineCamera. Measured: twin and live camera
  agree on focal 30 mm, filmback 23.0 x 18.66, overscan 0.08 at runtime.
- Keys on every frame, auto-tangent cubic: actor Transform (= the BEC camera component's world transform, rotation
  unwound), and on the twin's camera-component binding `CurrentFocalLength`, `FocusSettings.ManualFocusDistance`,
  `CurrentAperture`. Measured: twin playback equals the baked samples to 0.0000 at every frame checked.
- Bake info (date, range, sub-steps, warm-up, Black Eye version, components copied) lives in the twin template's
  actor Tags, so it undoes with the keys. Package metadata isn't transacted in 5.8.

**Lock / unlock.** Lock repoints the shot's camera cut sections from the BEC to the twin; unlock repoints them back.
Locked = a cut section points at the twin; no other state. The BEC stays untouched, so unlock returns the live camera.
Every edit or master that nests the shot plays and renders whichever is locked. Bake, lock and unlock are each one undo
step (measured, including undoing bakes on production shots).

**Measured on a production edit** (a scene's demo edit cutting between three angle shots, viewport locked to camera
cuts, realtime playback, camera speed in the 14 frames after each cut):

| Cut | Unlocked (live Black Eye) | Locked (twins) |
|---|---|---|
| 1 | 0.11 deg/frame | 0.29-0.43 deg/frame (the shot's own opening move) |
| 2 | 0.06 deg/frame | 0.12-0.16 deg/frame |
| 3 | **3.16 deg/frame, 73 cm/frame** | 0.33-0.37 deg/frame |
| 4 | **3.60 deg/frame, 56 cm/frame** | 0.20-0.24 deg/frame |

Mid-shot camera speed is about 0.04-0.06 deg/frame. Unlocked, the jolts are 50-90x that; a second unlocked run showed
none at those cuts, because live Black Eye depends on leftover camera state. Locked runs repeat.

Not built yet: re-sync settings without re-baking (P2); keeping the previous bake for comparison.

## 6. Entry points

*Status: planned.*

- **Sequencer binding right-click** (built): Black Eye Fast Bake ▸ Bake and lock / Re-bake and lock, Lock / Unlock,
  with the bake info in the tooltips and a toast with the result (`BlackEyeFastBakeMenu.cpp`).
  **Measured crash (2026-10-06), fixed:** building the submenu called `OpenEditorForAsset` to find the binding. With a
  shot focused inside an edit, that opened the shot as a new root, destroying the Sequencer that owned the open menu,
  and Slate asserted (`SharedPointer.h` `IsValid()` from `MenuStack.cpp`). Now building a menu has no side effects (it
  finds the binding through the Sequencers already open, tracked via `ISequencerModule::RegisterOnSequencerCreated`),
  and actions run on the next tick. A bake started from inside an edit opens the shot alone, bakes, then reopens the
  edit and focuses back into the shot. `BlackEyeCustom.FastBake.MenuTest` builds the menu off screen for every Black
  Eye camera in the open Sequencers.
  **Second measured crash (2026-10-06), fixed:** the bake opened the shot as a new root Sequencer and wrote the twin in
  the same tick; that Sequencer had no track editors yet ("Unable to find a track editor for track type
  MovieSceneFloatTrack") and asserted when the bake's transaction closed. Now a sequence opened for a bake gets half a
  second to settle first (`SequencerSettleSeconds`), the view restore does the same, and `BakeShot` from Python refuses
  (opens it, asks to run again) rather than editing a sequence opened in the same call. `BlackEyeCustom.FastBake.Bake
  <binding>` runs the menu's Bake and lock from the console; it replayed the crash case cleanly.
- **Content Browser:** right-click shot Level Sequence(s) ▸ Bake Black Eye cameras, in batch (all angles at once).
- **`UBlackEyeFastBakeLibrary`** (built; BlueprintCallable, so Python and agents can drive it): `BakeShot(LS, Options)`,
  `SetLocked(LS, CameraBindingName, bool)`, `GetBakeInfo(LS)`, plus `BakeCameraToCsv` and the realtime record for
  measuring.
- **`BlackEyeCustom.SelfTest`** console command (built: checks the Black Eye contract, §3).

## 7. What becomes trivial inside Black Eye

*Status: planned; grows as workarounds are written. `grep -rn BE-NATIVE Source/` is the port checklist.*

- **A frame-rate-independent LookAt/Follow step** (trap 4.12): editor playback, renders and bakes would then agree
  on framing at any tick rate.

- **A public step-camera function** (`StepCamera(dt)`), so a baker never needs the whole actor tick, tick disabling,
  or reflection.
- **A correct editor camera-cut snap**: fire `NotifyCameraCut` in the editor on every straight cut and on cuts back
  to the same camera, and re-snap once subjects resolve. This alone removes most of the jolt without any bake.
- **The Bake button revived on `LinkedCamera`'s copy list**: `LinkedCamera` already knows every property worth
  baking; an offline loop over it is the whole feature.

## 8. Measured numbers

*Status: measured 2026-10-06 on the repro (`Tools/fast_bake_repro.py`: one walking mannequin, one BEC with Follow
and a head-bone LookAt, 300 frames at 30 fps), UE 5.8.2, Black Eye 2.0.7, Ryzen 9 9950X3D. Raw tracks and a yaw plot
are in `data/`; `Tools/compare_bake.py` reproduces every number below. A production angle (many meshes, MetaHumans)
is measured separately below.*

**Speed** (1 tick per frame, subject meshes only; the scene has one skeletal mesh):

| Measure | Value |
|---|---|
| Total per frame | 0.27-0.39 ms |
| Sequencer evaluation | 0.02-0.10 ms |
| Subject mesh refresh (1 mesh) | 0.07-0.17 ms |
| Camera tick | 0.01 ms |
| Speed | **90-124x realtime** (300 frames in 0.08-0.12 s) |
| Each extra sub-step | about +0.3 ms/frame (4 sub-steps: 1.0 ms/frame, 34x) |

The reviewer had estimated about 10 ms/frame (4x). Go/no-go was 10x realtime: **go** on the repro. The shared
subject pass (section 9) stays parked until a production angle is measured. Trap 4.7 (`GFrameCounter`) did not show
up as a cost: Sequencer's skeletal system never force-refreshed a mesh inside the loop (`PoseTickedThisFrame` stayed
false).

**Correctness.** The reference is a realtime record of the same shot: editor playback at about 120 fps (about 4
ticks per frame), with the camera sampled after every world tick (`StartRealtimeRecord`) and interpolated to each
baked frame.

| Track vs realtime playback | Position mean / max | Rotation mean / max |
|---|---|---|
| Realtime run 2 vs run 1 (noise floor) | 0.005 / 0.078 cm | 0.009 / 0.161 deg |
| Bake, 1 tick per frame | 1.815 / 2.552 cm | 0.269 / 1.064 deg |
| Bake, 2 ticks per frame | 0.616 / 0.888 cm | 0.077 / 0.167 deg |
| **Bake, 4 ticks per frame** | **0.085 / 0.324 cm** | **0.009 / 0.129 deg** |
| Bake, 8 ticks per frame | 0.315 / 0.575 cm | 0.041 / 0.137 deg |
| Subject head bone, bake vs realtime | 0.00-0.10 cm | |

Once the tick rate matches (4 ticks, about the viewport's 120 fps), the bake reproduces live Black Eye to realtime's
own noise, damping included: realtime playback never snaps mid-shot. The 1-tick differences are trap 4.12, not a bake
error. Focal length was constant in this shot (no Dynamic FoV), so focal keys are untested.

**Production angle** (2026-10-06): a 30 fps shot about 42,000 frames (23 min) long holding a whole scene assembly:
19 skeletal meshes including several MetaHumans, the subject a tracker on a MetaHuman eye, the camera carrying
DynamicLens with overscan. 300-600 frame ranges, 1 tick per frame:

| Measure | Value |
|---|---|
| Total per frame | 3.3-3.9 ms |
| Sequencer evaluation (whole assembly) | 1.0-2.8 ms |
| Subject mesh refresh (10 meshes up the attach chains) | 0.6-1.0 ms |
| Refresh of all 19 skeletal meshes instead | 2.0 ms |
| Camera tick + components | 0.03-0.06 ms |
| Speed | **8.6-10.2x realtime**; the full 23-minute angle in about 2.6 min |

Just under the 10x go line, because evaluating the whole assembly dominates. That is the case the shared subject pass
(section 9) was parked for; it moves into P1 as an option, not a blocker (2.6 min an angle already beats 23).

Correctness on this rig: live playback is **not repeatable**. Two realtime runs of the same range differ by 5.0 cm /
5.9 deg mean (max 35 cm / 9 deg), because editor frame rate varies and trap 4.12 makes the result depend on it. The
bake differs from those two runs by 0.45 cm / 0.17 deg and 5.9 cm / 6.0 deg mean, so it lies within live playback's
own spread, and it repeats (trap 4.15). Locking a bake is the only way this shot plays the same way twice.

## 9. Decisions and rejected ideas

- **Extension, not a fork of Black Eye** (2026-10-06). Everything the bake needs is reachable from outside: `Tick` is
  virtual, the snap is a UFUNCTION, the rest is reflected properties. A fork would carry Black Eye's source, which
  can't be redistributed, would be overwritten by every Fab update, and would bury the change in a large diff. As an
  extension, the changes Black Eye would make natively are listed in §7 and tagged `BE-NATIVE` in the source.
  Revisit only if a needed piece of state is unreachable without patching.
- **Not linking `Black_Eye`** (provisional; decide in P0). Pro: a Fab update never forces a rebuild, and the plugin
  loads without Black Eye. Con (reviewer): reflection on target structs is as fragile as a link. Mitigation: one
  contract file and a self-test that names any broken symbol.
- **Not plugging into AutoBake's `ISequencerBakeRecorder`.** Its signature-triggered re-bakes run from the editor
  tick and would corrupt the BEC's damping state between bakes. The loop pattern is copied instead.
- **Rejected: Take Recorder through `LinkedCamera`** as the main workflow. Realtime only; it's the baseline to beat.
- **Rejected: Sequencer Bake Transform / Bake to Control Rig.** They never tick the actor and don't key focal length
  (§2).
- **Rejected: a Python bake.** Python can't step a BEC (§2).
- **Rejected: a headless batch.** LookAt needs a live viewport (trap 4.4).
- **Speed fallback: a shared subject pass** (moves into P1 if P0 measures under 10× realtime).
  1. Evaluate the scene once and cache tracker and Follow-target world transforms per frame.
  2. Per angle, mute the scene, drive the targets from the cache, evaluate only the camera's own tracks, then tick.
  3. Estimated under 1 ms/frame: seconds per angle.

## 10. Phases

- **P0 spike:** *done 2026-10-06*, on the repro and on a production angle. Results in section 8, new traps
  4.11-4.15. Answered: tick-disable plus manual `Tick` works; tracker and bone targets update inside the blocking
  loop (legacy anim path only, 4.11); damping survives with camera cuts off; LookAt sees a valid viewport inside a
  blocking loop driven from Python under a modal slow task. Production speed is 9-10x realtime (shared subject
  pass optional in P1). The realtime record replaced the `LinkedCamera` Take Recorder comparison as the reference.
  The original list:
  - Measure ms/frame, whole-assembly evaluation vs subject-mesh refresh; profile which meshes refresh per evaluation.
  - Check the tick-disable plus manual `Tick` path, and that tracker/bone targets update inside the blocking loop.
  - Confirm damping survives with the camera-cut handler disabled.
  - Compare the baked curve against a realtime `LinkedCamera` Take Recorder record.
  - **Go/no-go on ≥10× realtime.** Lower, and the shared subject pass moves into P1.
  - Save the numbers to `docs/fast-bake/data/` and §8.
- **P1 MVP:** *done 2026-10-06.* Repro script; `BE-NATIVE` tags; README section; bake into the twin (full range
  or a frame range with warm-up); lock/unlock; binding menu and library; non-Black-Eye components (DynamicLens);
  cancel/progress; undo. Full-range bakes of three production angles: 87k frames in 3.8 min (12-24x realtime).
- **Alignment (2026-10-06):** Dylan reported the twin didn't line up with the live camera (it sat further back).
  Against *playback* the twin matches to 1.1 cm / 0.1 deg (trap 4.16). Two causes remain: a parked live camera has
  caught up with its subject while playback lags by its damping (4.16), and re-bakes of a locked shot kept playing an
  older twin (4.17, fix built, not yet installed or verified).
- **P2 batch and UX:** Content Browser batch over a shots folder; bake info (date, range, BEC parameter hash); a stale
  flag when BEC tracks or subject sections change (reusing AutoBake's track-signature idea); re-sync settings.
- **P3 speed** (only if P0 numbers need it): edit-aware partial bakes (only the ranges each shot is cut into, plus
  warm-up); the shared subject cache across angles; AutoBake re-bake on change.

**The repro for the Black Eye team** (P1): a tiny map and sequence with a moving Manny, one BEC and a two-shot edit,
showing the jolt and the fix in about two minutes. No MetaHumans, no project content.

## 11. Open questions

- Does the twin play correctly when an edit nests the shots? Test on a short edit with a few cuts first, then a full
  master sequence. The primary goal is batch-baking every shot at full extent; edit-aware partial bakes wait for P3.
- ~~Does `TickLookAt` see a valid viewport inside a blocking loop?~~ Yes when driven from Python under a modal slow
  task (P0). A Slate menu entry point is untested.
- Which tick rate should be the default: render-faithful (`SubSteps` = the render's Temporal Sample Count) or
  viewport-faithful (what the user saw while tweaking)? Default 1, render-faithful (trap 4.12).
- Support the Anim Mixer path, or refuse to bake when a subject uses it (trap 4.11)?
- Does a MetaHuman face (a tracker on `Face/FACIAL_L_Eye`) need its post-process AnimBP ticked to move the eye bone?
  Check that the eye bone matches between normal playback and the bake.
- Link `Black_Eye` or not (§9).

## 12. Verification

- **Correctness:** bake one angle, lock it, scrub or jump anywhere in the shot: the framing equals full-shot playback
  at that frame. Check the component world transform and focal length against the baked keys at 5 random frames.
- **The edit:** with every angle an edit uses locked, there's no jolt at any cut, whether scrubbing, jumping or
  playing.
- **Round trip:** unlock and the live BEC returns; Ctrl+Z reverts a bake.
- **Render:** a Movie Render Graph render of a locked shot matches the editor.
- **Speed:** log ms/frame and total time for one full 30-minute angle.
