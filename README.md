<!-- Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
     SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0 -->
# Black Eye Custom

Editor extensions for the [Black Eye](https://www.youtube.com/@BlackEyeTechnologies) camera plugin for Unreal
Engine 5.8, by Dylan Gitalis (Mad Rice).

Each extension solves a problem we hit in production, and is written to be read: the reasoning, the source it
rests on and the traps found along the way are all in the repo, so the Black Eye developers can rebuild it their
own way inside Black Eye. It is an extension, not a fork: no Black Eye code is copied or patched.

## Extensions

| Extension | What it fixes | Status | Design |
|---|---|---|---|
| **Fast Bake** | Black Eye cameras jolt into frame at every cut when an edit is played in the editor, because their damping runs on wall-clock ticks, not the playhead. Fast Bake solves each camera offline, faster than realtime, into keys on a plain CineCamera "twin" in the same shot, and locks the shot to it (or unlocks it back to the live camera). | Working: Sequencer menu + Python. about 22x realtime on a minute of a production shot (2026-10-06); a locked edit plays without jolts | [docs/fast-bake/DESIGN.md](docs/fast-bake/DESIGN.md) |

## For the Black Eye team

Three places give the whole picture:

1. **`docs/<extension>/DESIGN.md`**: the problem, its root cause cited to Black Eye and engine source by `file:line`
   (pinned to Black Eye 2.0.7 / UE 5.8.2), the algorithm, every trap, rejected ideas with reasons, measured numbers,
   and a section on what becomes trivial inside Black Eye.
2. **`grep -rn BE-NATIVE Source/`**: every workaround carries a `// BE-NATIVE: <native alternative> (<file:line>)`
   comment. Together they are the port checklist.
3. **`Source/BlackEyeCustomEditor/Public/BlackEyeContract.h`**: every Black Eye class, function and property this
   plugin uses. It reaches Black Eye through reflection rather than linking it, so this one file is the whole
   dependency surface. The `BlackEyeCustom.SelfTest` console command checks each entry.

## Using Fast Bake

- **In Sequencer:** open a shot, right-click the Black Eye camera's binding > **Black Eye Fast Bake** > **Bake and
  lock**. It bakes the whole playback range into `<camera>_Bake` and makes the shot's camera cuts play it. The same
  menu then offers **Unlock** (play the live camera) and **Lock**. Every action is one undo step.
- **From Python:** `unreal.BlackEyeFastBakeLibrary.bake_shot(sequence, options)` (options: `camera_binding_name`,
  `start_frame`, `end_frame`, `warm_up_frames`, `sub_steps`, `lock_after_bake`), `set_locked(sequence, name, bool)`,
  `get_bake_info(sequence)`.
- A frame range bakes only that range; outside it the twin holds its first and last keys. Bake the whole shot (the
  default) before relying on a lock everywhere. Starting mid-shot, give `warm_up_frames` (a few seconds) so the damping
  has settled.
- Needs the editor with a level viewport (Black Eye's LookAt reads it). Nothing headless.

## Reproduce Fast Bake

1. In any UE 5.8 project with Black Eye and this plugin enabled, run `Tools/fast_bake_repro.py` in the editor
   (`py "<path>/fast_bake_repro.py"`, add `--mesh`/`--anim` if the Third Person mannequin is elsewhere). It builds a
   level and a 300-frame shot with a walking mannequin and one spawnable Black Eye camera.
2. Bake from Python:
   `unreal.BlackEyeFastBakeLibrary.bake_camera_to_csv(unreal.load_asset("/Game/FastBakeRepro/LS_FastBakeRepro_A"), options)`
   with `options = unreal.BlackEyeFastBakeOptions()` and `options.csv_path` set.
3. For a live reference: `start_realtime_record(seq, "")`, play the sequence, then `stop_realtime_record(csv_path)`.
4. `python Tools/compare_bake.py <realtime.csv> <bake.csv>` prints the differences (`docs/fast-bake/DESIGN.md` section 8).

## Requirements

- Unreal Engine 5.8, Win64, editor only.
- Black Eye 2.0.7 (another 2.0.x loads, and the self-test warns until it has been checked).

## Install

Pick one. A plugin found at both an engine path and a project path fails to load.

- **Per project:** clone into `<YourProject>/Plugins/BlackEyeCustom/` and open the project; it offers to compile.
- **Every project on the machine:** junction this folder into the engine with `Tools\install_junction.ps1`
  (creates `Engine\Plugins\Marketplace\BlackEyeCustom`), then build with `Tools\build_blackeyecustom.ps1`.

Then run `BlackEyeCustom.SelfTest` in the editor console. Every line must end in `ok`.

## Build

```powershell
Tools\build_blackeyecustom.ps1 -Status       # what is built, installed, and next
Tools\build_blackeyecustom.ps1               # build into a temp package (safe with the editor open)
Tools\build_blackeyecustom.ps1 -InstallOnly  # copy the build in (editor closed)
```

Needs Visual Studio's C++ toolchain and the .NET Framework 4.8 SDK.

## License

Source-available under CPAL-1.0 with the Commons Clause, the same as Dylan Gitalis's other Unreal plugins. See
`LICENSE` and `NOTICE`. In short, anyone may use it for anything, monetized films included, and modify it privately.
Anyone who distributes it keeps the source open and the credit visible. No one may sell it.

**Black Eye Technologies** has a separate permission in `LICENSE` to build any of this into its own products, paid
ones included, with none of those conditions. Credit is welcome but not required. The permission is theirs alone.

Black Eye is a product of Black Eye Technologies. This repository contains none of its source; it cites files and
line numbers that Black Eye licensees can read in their own install.
