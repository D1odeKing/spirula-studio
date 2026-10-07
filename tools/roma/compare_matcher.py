"""Manually compare native coarse matching to the pinned RoMa FP32 reference."""

import argparse
import contextlib
import sys
from pathlib import Path
from unittest.mock import patch

import numpy as np
import torch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--roma-source", required=True)
    parser.add_argument("--native-dump", required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(Path(args.roma_source) / "src"))
    from romav2.matcher import Matcher

    torch.set_num_threads(8)
    torch.set_float32_matmul_precision("highest")
    model = Matcher(Matcher.Cfg(enable_amp=False)).eval().float()
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    model.load_state_dict({k[8:]: v for k, v in checkpoint.items() if k.startswith("matcher.")}, strict=True)
    del checkpoint
    model.mv_vit.rope_embed.dtype = torch.float32
    failed = False
    for h, w in [(2, 3), (3, 5)]:
        features = []
        for view in range(2):
            taps = []
            for tap in range(2):
                i = np.arange(h * w * 1024)
                values = ((i * 13 + tap * 97 + view * 37) % 511 - 255).astype(np.float32) / 255
                taps.append(torch.from_numpy(values.reshape(1, h, w, 1024)))
            features.append(taps)
        image = torch.zeros(1, 3, h * 16, w * 16)
        with torch.inference_mode(), patch("torch.autocast", lambda *a, **kw: contextlib.nullcontext()):
            head = model.head(features[0])
            preds = model([x.clone() for x in features[0]], [x.clone() for x in features[1]],
                          img_A=image, img_B=image, bidirectional=True)
        outputs = {"head": head}
        for direction in ["AB", "BA"]:
            outputs[direction] = torch.cat((preds[f"warp_{direction}"], preds[f"confidence_{direction}"]), -1)
        for name, ref in outputs.items():
            expected = ref.numpy().reshape(-1).astype(np.float64)
            actual = np.fromfile(Path(args.native_dump) / f"coarse_{h}x{w}_{name}.f32", dtype=np.float32).astype(np.float64)
            if actual.shape != expected.shape:
                raise ValueError(f"{name}: shape {actual.shape}, expected {expected.shape}")
            relative_l2 = np.linalg.norm(actual - expected) / np.linalg.norm(expected)
            maximum = np.max(np.abs(actual - expected))
            passed = bool(np.isfinite(actual).all() and relative_l2 < 2e-4)
            failed |= not passed
            print(f"{'PASS' if passed else 'FAIL'} {h}x{w} {name}: relative L2 {relative_l2:.8g}, max absolute {maximum:.8g}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
