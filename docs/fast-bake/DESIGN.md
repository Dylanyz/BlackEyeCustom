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
| [3. The algorithm](#3-the-algorithm) | built (P0: samples to CSV; keys are P1) |
| [4. Traps](#4-traps) | 4.11 and 4.12 measured in P0; 4.1 and 4.4 confirmed; the rest from source and review |
| [5. Output: the baked twin, lock and unlock](#5-output-the-baked-twin-lock-and-unlock) | planned |
| [6. Entry points](#6-entry-points) | planned |
| [7. What becomes trivial inside Black Eye](#7-what-becomes-trivial-inside-black-eye) | planned |
| [8. Measured numbers](#8-measured-numbers) | measured on the repro (P0); a production angle pending |
| [9. Decisions and rejected ideas](#9-decisions-and-rejected-ideas) | live |
| [10. Phases](#10-phases) | P0 done on the repro; P1 next |
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

*Status: built as the P0 spike (`Source/BlackEyeCustomEditor/Private/FastBake/BlackEyeFastBake.cpp`,
`UBlackEyeFastBakeLibrary::BakeCameraToCsv`). It samples to CSV; writing keys onto the twin (step 5) is P1.*

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
   2. Refresh only the **subject** skeletal meshes (those the LookAt / Follow / Focus targets attach to, plus their
      leader and follower meshes), in leader-first order (trap 4.6). Evaluate constraints again.
   3. `BEC->Tick(dt)`: the virtual `AActor::Tick`, whose BEC override runs Follow then LookAt.
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

## 5. Output: the baked twin, lock and unlock

*Status: planned.*

**The twin** (decided 2026-10-06). Every bake creates a fresh spawnable plain `ACineCameraActor` beside the BEC in the
same shot, named `<BEC label>_Bake`.
- A re-bake replaces the previous twin; optionally the old one is kept, muted, for comparison.
- Camera settings (filmback, lens, overscan, crop, post process, focus method) are copied from the BEC, plus
  non-Black-Eye components on it (for example a lens-model component). Check that instance components survive the
  spawnable template.
- Keys: the Transform track (actor = camera component world), `CurrentFocalLength`, `FocusSettings.ManualFocusDistance`.

**Lock / unlock.** State lives as asset metadata on the shot plus a tag on the twin binding.
- Lock: repoint the shot's camera cut track to the twin. The BEC stays in the shot, untouched.
- Unlock: repoint it back to the BEC. The twin's keys stay, for comparison and re-bake.
- Because the bake lives in the shot, every edit or master sequence that nests the shot plays and renders the baked
  camera.
- Re-sync settings: copy non-motion settings from the BEC to the twin without re-baking.

## 6. Entry points

*Status: planned.*

- **Sequencer binding right-click:** Black Eye ▸ Bake / Lock / Unlock / Re-bake.
- **Content Browser:** right-click shot Level Sequence(s) ▸ Bake Black Eye cameras, in batch (all angles at once).
- **`UBlackEyeBakeLibrary`** (BlueprintCallable, so Python and agents can drive it): `BakeShot(LS, Options)`,
  `SetLocked(LS, bool)`, `GetBakeInfo(LS)`.
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
is still to measure.*

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

- **P0 spike:** *done 2026-10-06 on the repro, not yet on a production angle.* Results in section 8, new traps
  4.11 and 4.12. Answered: tick-disable plus manual `Tick` works; tracker and bone targets update inside the blocking
  loop (legacy anim path only, 4.11); damping survives with camera cuts off; LookAt sees a valid viewport inside a
  blocking loop driven from Python under a modal slow task. Still open: a production angle's ms/frame, and a
  `LinkedCamera` Take Recorder comparison (the realtime record above replaced it as the reference). The original list:
  - Measure ms/frame, whole-assembly evaluation vs subject-mesh refresh; profile which meshes refresh per evaluation.
  - Check the tick-disable plus manual `Tick` path, and that tracker/bone targets update inside the blocking loop.
  - Confirm damping survives with the camera-cut handler disabled.
  - Compare the baked curve against a realtime `LinkedCamera` Take Recorder record.
  - **Go/no-go on ≥10× realtime.** Lower, and the shared subject pass moves into P1.
  - Save the numbers to `docs/fast-bake/data/` and §8.
- **P1 MVP:** a Manny repro map; every workaround tagged `BE-NATIVE`; README section; full-range bake into the twin;
  lock/unlock; binding menu and library; copying non-Black-Eye components; cancel/progress; undo.
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
