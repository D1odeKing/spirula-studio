"""Manually compare native float RGB antialiased resizing at every RoMa preset size."""

import argparse
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-dump", required=True)
    args = parser.parse_args()
    torch.set_num_threads(8)
    y, x, c = np.indices((960, 1280, 3))
    failed = False
    for pattern in ["textured", "aliasing"]:
        xx, yy = (x, y) if pattern == "aliasing" else (x // 8, y // 8)
        rgb = ((17 * xx + 31 * yy + 73 * c) % 256).astype(np.float32) / 255
        for w, h in [(320, 320), (512, 512), (640, 640), (800, 800), (1280, 1280), (48, 32)]:
            name = f"{pattern}_{w}x{h}"
            want = F.interpolate(torch.from_numpy(rgb).permute(2, 0, 1)[None], size=(h, w),
                                 mode="bicubic", align_corners=False, antialias=True).permute(0, 2, 3, 1).numpy().reshape(-1)
            got = np.fromfile(Path(args.native_dump) / f"{name}.f32", dtype=np.float32)
            if got.shape != want.shape:
                raise ValueError(f"{name}: wrong output shape")
            maximum = np.max(np.abs(got - want))
            passed = np.isfinite(got).all() and maximum <= 2e-6
            failed |= not passed
            print(f"{'PASS' if passed else 'FAIL'} {name}: max absolute {maximum:.8g}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
