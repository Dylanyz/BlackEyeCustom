# SPDX-License-Identifier: Apache-2.0
"""
Builds the Fast Bake repro in any UE 5.8 project with Black Eye: a level with a walking Manny on a keyed path,
and a shot Level Sequence with one spawnable Black Eye camera that Follows Manny and Looks At its head bone.
Run it in the editor (Tools > Execute Python Script, or `py "<path>" [args]` in the console).

    fast_bake_repro.py [--folder /Game/FastBakeRepro] [--mesh <SkeletalMesh path>] [--anim <AnimSequence path>]

Defaults are the Third Person template's mannequin. Re-running reuses what exists; delete the folder to rebuild.
"""
import argparse
import sys

import unreal

parser = argparse.ArgumentParser()
parser.add_argument("--folder", default="/Game/FastBakeRepro")
parser.add_argument("--mesh", default="/Game/Characters/Mannequins/Meshes/SKM_Manny")
parser.add_argument("--anim", default="/Game/Characters/Mannequins/Animations/Manny/MM_Walk_InPlace")
parser.add_argument("--frames", type=int, default=300)
args = parser.parse_args(sys.argv[1:])

ext = unreal.MovieSceneSequenceExtensions
assets = unreal.EditorAssetLibrary
level_path = f"{args.folder}/L_FastBakeRepro"
seq_path = f"{args.folder}/LS_FastBakeRepro_A"
FPS = 30
N = args.frames

mesh = unreal.load_asset(args.mesh)
anim = unreal.load_asset(args.anim)
if not mesh or not anim:
    raise RuntimeError(f"mesh or anim not found: {args.mesh} / {args.anim}")

# --- level: a floor, a light, and Manny --------------------------------------------------------------------
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if assets.does_asset_exist(level_path):
    les.load_level(level_path)
else:
    les.new_level(level_path)
    floor = actors.spawn_actor_from_object(unreal.load_asset("/Engine/BasicShapes/Plane"), unreal.Vector(0, 0, 0))
    floor.set_actor_scale3d(unreal.Vector(60, 60, 1))
    floor.set_actor_label("Floor")
    sun = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 500), unreal.Rotator(0, -50, 30))
    sun.set_actor_label("Sun")
    actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 300)).set_actor_label("Sky")
    manny = actors.spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0))
    manny.set_actor_label("Manny")
    manny.skeletal_mesh_component.set_skeletal_mesh_asset(mesh)
    les.save_current_level()

manny = next(a for a in actors.get_all_level_actors() if a.get_actor_label() == "Manny")

# --- the shot sequence (each part is added only if missing, so a re-run completes a partial build) -------------
lses = unreal.get_editor_subsystem(unreal.LevelSequenceEditorSubsystem)
if assets.does_asset_exist(seq_path):
    seq = unreal.load_asset(seq_path)
else:
    folder, name = seq_path.rsplit("/", 1)
    seq = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    ext.set_display_rate(seq, unreal.FrameRate(FPS, 1))
    ext.set_playback_start(seq, 0)
    ext.set_playback_end(seq, N)
unreal.LevelSequenceEditorBlueprintLibrary.open_level_sequence(seq)
bindings = {b.get_name(): b for b in ext.get_bindings(seq)}

if "Manny" not in bindings:
    # Manny walks a bent path, turning to face where he goes. Keys in display frames.
    mb = ext.add_possessable(seq, manny)
    xf = mb.add_track(unreal.MovieScene3DTransformTrack).add_section()
    xf.set_range(0, N)
    ch = xf.get_all_channels()  # Location X Y Z, Rotation X(roll) Y(pitch) Z(yaw), Scale X Y Z
    path = [(0, 0, 0, 0), (N // 3, 500, 0, 0), (2 * N // 3, 900, 400, 70), (N, 950, 1000, 90)]
    for frame, x, y, yaw in path:
        f = unreal.FrameNumber(frame)
        ch[0].add_key(f, x)
        ch[1].add_key(f, y)
        ch[2].add_key(f, 0.0)
        ch[5].add_key(f, yaw)
    walk = mb.add_track(unreal.MovieSceneSkeletalAnimationTrack).add_section()
    walk.set_range(0, N)
    params = walk.get_editor_property("params")
    params.set_editor_property("animation", anim)
    walk.set_editor_property("params", params)

cam = bindings.get("BEC_A")
if cam is None:
    cam = lses.add_spawnable_from_class(unreal.BlackEyeCineCameraActor)
    cam.set_name("BEC_A")

# One spawnable Black Eye camera: Follow Manny, Look At his head bone. Re-applied every run (cheap, idempotent).
tmpl = cam.get_object_template()
tmpl.get_editor_property("root_component").set_editor_property("relative_location", unreal.Vector(-400, -400, 160))
look = tmpl.get_editor_property("look_at")
target = look.get_editor_property("target_0")
target.set_editor_property("actor", manny)
target.set_editor_property("auto_size", False)
target.set_editor_property("bone_name", "head")
look.set_editor_property("target_0", target)
follow = tmpl.get_editor_property("follow")
ftarget = follow.get_editor_property("target_0")
ftarget.set_editor_property("actor", manny)
follow.set_editor_property("target_0", ftarget)

if not any(isinstance(t, unreal.MovieSceneCameraCutTrack) for t in ext.get_tracks(seq)):
    cut = ext.add_track(seq, unreal.MovieSceneCameraCutTrack).add_section()
    cut.set_range(0, N)
    cut.set_camera_binding_id(ext.get_portable_binding_id(seq, seq, cam))

assets.save_asset(seq_path)
print(f"level {level_path}, sequence {seq_path}, {N} frames at {FPS} fps")
