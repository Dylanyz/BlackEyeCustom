# Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
# SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
"""
Compares camera tracks written by UBlackEyeFastBakeLibrary (BakeCameraToCsv / StopRealtimeRecord).
Plain Python 3, no dependencies.

    compare_bake.py <reference.csv> <candidate.csv> [<candidate.csv> ...] [--svg yaw.svg]

The reference is sampled at each candidate frame by linear interpolation (a realtime record has sub-frame
timestamps). Prints position / rotation / focal error stats per candidate; --svg plots camera yaw over frames.
"""
import argparse
import csv
import math


def load(path):
    with open(path, newline="") as f:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f)]


def sample(track, frame, hint=0):
    """Linear interpolation of a track at a frame; returns (row, next hint) or (None, hint) outside it."""
    j = hint
    while j + 1 < len(track) and track[j + 1]["frame"] < frame:
        j += 1
    if j + 1 >= len(track) or not (track[j]["frame"] <= frame <= track[j + 1]["frame"]):
        return None, j
    a, b = track[j], track[j + 1]
    t = (frame - a["frame"]) / (b["frame"] - a["frame"]) if b["frame"] > a["frame"] else 0.0
    row = {k: a[k] + (b[k] - a[k]) * t for k in a if k in b}
    n = math.sqrt(sum(row[k] ** 2 for k in ("qx", "qy", "qz", "qw")))
    for k in ("qx", "qy", "qz", "qw"):
        row[k] /= n
    return row, j


def angle_deg(a, b):
    dot = abs(sum(a[k] * b[k] for k in ("qx", "qy", "qz", "qw")))
    return math.degrees(2 * math.acos(min(1.0, dot)))


def compare(ref, cand):
    errs, hint = [], 0
    for c in cand:
        r, hint = sample(ref, c["frame"], hint)
        if r is None:
            continue
        errs.append((math.dist((c["x"], c["y"], c["z"]), (r["x"], r["y"], r["z"])),
                     angle_deg(c, r), abs(c["focal"] - r["focal"])))
    return errs


def stats(values):
    v = sorted(values)
    return f"mean {sum(v) / len(v):.4f}  p95 {v[int(0.95 * (len(v) - 1))]:.4f}  max {v[-1]:.4f}"


def svg_plot(series, path, key="yaw", w=900, h=360, pad=50):
    """series: [(label, rows)]. Plots rows[key] against frame."""
    xs = [r["frame"] for _, rows in series for r in rows]
    ys = [r[key] for _, rows in series for r in rows]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    sx = lambda x: pad + (x - x0) / (x1 - x0 or 1) * (w - 2 * pad)
    sy = lambda y: h - pad - (y - y0) / (y1 - y0 or 1) * (h - 2 * pad)
    colors = ["#888888", "#d1495b", "#2e86ab", "#3b8b5a", "#e09f3e"]
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" font-family="sans-serif" font-size="12">',
           f'<rect width="{w}" height="{h}" fill="white"/>',
           f'<text x="{pad}" y="20">camera {key} (deg) vs frame</text>',
           f'<text x="{pad}" y="{h - 15}">{x0:.0f}</text><text x="{w - pad - 20}" y="{h - 15}">{x1:.0f}</text>',
           f'<text x="5" y="{pad}">{y1:.2f}</text><text x="5" y="{h - pad}">{y0:.2f}</text>']
    for i, (label, rows) in enumerate(series):
        pts = " ".join(f"{sx(r['frame']):.1f},{sy(r[key]):.1f}" for r in rows)
        c = colors[i % len(colors)]
        out.append(f'<polyline fill="none" stroke="{c}" stroke-width="1.5" points="{pts}"/>')
        out.append(f'<text x="{w - pad - 220}" y="{40 + 16 * i}" fill="{c}">{label}</text>')
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("reference")
    p.add_argument("candidates", nargs="+")
    p.add_argument("--svg")
    a = p.parse_args()
    ref = load(a.reference)
    series = [(a.reference.rsplit("/", 1)[-1], ref)]
    for path in a.candidates:
        cand = load(path)
        series.append((path.rsplit("/", 1)[-1], cand))
        e = compare(ref, cand)
        print(f"{path}: {len(e)} frames")
        print(f"  position cm   {stats([x[0] for x in e])}")
        print(f"  rotation deg  {stats([x[1] for x in e])}")
        print(f"  focal mm      {stats([x[2] for x in e])}")
    if a.svg:
        svg_plot(series, a.svg)
        print(f"wrote {a.svg}")
