<!-- SPDX-License-Identifier: Apache-2.0 -->
# Black Eye Custom

Editor extensions for the [Black Eye](https://www.youtube.com/@BlackEyeTechnologies) camera plugin for Unreal
Engine 5.8, by Dylan Gitalis (Mad Rice).

Each extension solves a problem we hit in production, and is written to be read: the reasoning, the source it
rests on and the traps found along the way are all in the repo, so the Black Eye developers can rebuild it their
own way inside Black Eye. It is an extension, not a fork: no Black Eye code is copied or patched.

## Extensions

| Extension | What it fixes | Status | Design |
|---|---|---|---|
| **Fast Bake** | Black Eye cameras jolt into frame at every cut when an edit is played in the editor, because their damping runs on wall-clock ticks, not the playhead. Fast Bake solves each camera offline, faster than realtime, into keys on a plain CineCamera that you can lock in and unlock. | P0 done: 90-124x realtime on the repro, 9-10x on a production angle (a 23-min angle in ~2.6 min), within live playback's own spread; keys, lock/unlock next (P1) | [docs/fast-bake/DESIGN.md](docs/fast-bake/DESIGN.md) |

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

Apache-2.0. See `LICENSE` and `NOTICE`; `NOTICE` must travel with any redistribution.

Black Eye is a product of Black Eye Technologies. This repository contains none of its source; it cites files and
line numbers that Black Eye licensees can read in their own install.
