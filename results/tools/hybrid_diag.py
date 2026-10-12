#!/usr/bin/env python3
"""Structure check for a hybrid model against a reference (the LoMa model).

    python results/tools/hybrid_diag.py REF_SPARSE_DIR MODEL_SPARSE_DIR [MODEL ...]

For each model: the track-length histogram; a Sim3 fit of its camera centres
onto the reference's (by image name) and the centre / rotation residuals; and,
with its points carried into the reference frame, the share lying near some
reference point (within 1% of the scene extent), by track length. Points with a
wrong depth along their rays land in empty space and score low.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "sfm"))
from colmap_io import qvec2rotmat, read_images, read_points3D  # noqa: E402
from eval_poses import umeyama  # noqa: E402


def load(d):
    d = Path(d)
    return read_images(d / "images.bin", with_points=False), read_points3D(d / "points3D.bin")


def centres(images):
    out = {}
    for im in images.values():
        R = qvec2rotmat(im["qvec"])
        out[Path(im["name"]).name.lower()] = (R, -R.T @ im["tvec"])
    return out


def rot_deg(R):
    return np.degrees(np.arccos(np.clip((np.trace(R) - 1) / 2, -1, 1)))


def voxel_keys(xyz, h):
    v = (np.floor(xyz / h).astype(np.int64) + (1 << 20)) & ((1 << 21) - 1)
    return (v[:, 0] << 42) | (v[:, 1] << 21) | v[:, 2]


def main():
    rimages, rpoints = load(sys.argv[1])
    rc = centres(rimages)
    rxyz = rpoints["xyz"]
    lo, hi = np.percentile(rxyz, [5, 95], axis=0)
    extent = float(np.linalg.norm(hi - lo))
    h = 0.01 * extent
    base = np.floor(rxyz / h).astype(np.int64)
    occ = []
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                occ.append(voxel_keys((base + [dx, dy, dz]) * h + h / 2, h))
    occ = np.unique(np.concatenate(occ))
    print(f"reference {sys.argv[1]}: {len(rc)} images, {len(rxyz)} points, extent {extent:.3g}")
    for path in [sys.argv[1]] + sys.argv[2:]:
        images, points = load(path)
        mc = centres(images)
        names = sorted(set(mc) & set(rc))
        src = np.array([mc[n][1] for n in names])
        dst = np.array([rc[n][1] for n in names])
        s, R, t = umeyama(src, dst)
        cerr = np.linalg.norm((s * (R @ src.T)).T + t - dst, axis=1) / extent
        rerr = np.array([rot_deg(rc[n][0] @ (mc[n][0] @ R.T).T) for n in names])
        xyz = (s * (R @ points["xyz"].T)).T + t
        near = np.isin(voxel_keys(xyz, h), occ)
        tl = points["track_len"]
        print(f"\n== {path}")
        print(f"  {len(names)} common images; centre error / extent: median {np.median(cerr):.4f}, "
              f"90% {np.percentile(cerr, 90):.4f}, max {cerr.max():.4f}")
        print(f"  rotation error deg: median {np.median(rerr):.3f}, 90% {np.percentile(rerr, 90):.3f}, "
              f"max {rerr.max():.3f}")
        print(f"  {len(xyz)} points, {near.mean() * 100:.1f}% near the reference structure")
        for lo_, hi_ in ((2, 2), (3, 3), (4, 5), (6, 10**9)):
            sel = (tl >= lo_) & (tl <= hi_)
            if sel.any():
                label = f"{lo_}" if lo_ == hi_ else (f"{lo_}-{hi_}" if hi_ < 10**9 else f"{lo_}+")
                print(f"    track {label:>4}: {sel.sum():>9} points ({sel.mean() * 100:5.1f}%), "
                      f"{near[sel].mean() * 100:5.1f}% near")


if __name__ == "__main__":
    main()
