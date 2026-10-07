# Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
# SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
"""
Builds two edits over the Fast Bake repro shot (run fast_bake_repro.py first), for testing Bake Edit:

    LS_FastBakeRepro_Edit       Cinematic Shot track, three cuts into the shot: edit [0,40) = shot [30,70),
                                [40,70) = shot [200,230), [70,90) = shot [60,80)
    LS_FastBakeRepro_EditOuter  a nested edit: [0,50) = LS_FastBakeRepro_Edit [20,70)

    fast_bake_edit_repro.py [--folder /Game/FastBakeRepro]

With 10 handle frames, Bake Edit on the edit keys shot frames [20,90) and [190,240); on the outer edit, [40,80)
and [190,240) (it sees the edit's [20,70): shot [50,70) and [200,230), plus handles; not the third cut).
Re-running rebuilds both edits.
"""
import argparse
import sys

import unreal

parser = argparse.ArgumentParser()
parser.add_argument("--folder", default="/Game/FastBakeRepro")
args = parser.parse_args(sys.argv[1:])

ext = unreal.MovieSceneSequenceExtensions
assets = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
FPS = 30

shot = unreal.load_asset(f"{args.folder}/LS_FastBakeRepro_A")
if not shot:
    raise RuntimeError("run fast_bake_repro.py first: LS_FastBakeRepro_A not found")


def make_edit(name, cuts):
    """cuts: (edit start, edit end, inner sequence, inner frame at edit start), display frames at 30 fps."""
    path = f"{args.folder}/{name}"
    if assets.does_asset_exist(path):
        assets.delete_asset(path)
    edit = tools.create_asset(name, args.folder, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    ext.set_display_rate(edit, unreal.FrameRate(FPS, 1))
    ext.set_playback_start(edit, 0)
    ext.set_playback_end(edit, max(c[1] for c in cuts))
    track = ext.add_track(edit, unreal.MovieSceneCinematicShotTrack)
    for start, end, inner, inner_start in cuts:
        section = track.add_section()
        section.set_sequence(inner)
        section.set_row_index(0)
        section.set_range(start, end)
        # The offset is in the inner sequence's ticks, from the inner playback start.
        res = ext.get_tick_resolution(inner)
        ticks_per_frame = res.numerator // (res.denominator * FPS)
        params = section.get_editor_property("parameters")
        params.set_editor_property("start_frame_offset", unreal.FrameNumber((inner_start - ext.get_playback_start(inner)) * ticks_per_frame))
        section.set_editor_property("parameters", params)
    assets.save_loaded_asset(edit)
    return edit


edit = make_edit("LS_FastBakeRepro_Edit", [(0, 40, shot, 30), (40, 70, shot, 200), (70, 90, shot, 60)])
make_edit("LS_FastBakeRepro_EditOuter", [(0, 50, edit, 20)])
print("built", f"{args.folder}/LS_FastBakeRepro_Edit", f"{args.folder}/LS_FastBakeRepro_EditOuter")
