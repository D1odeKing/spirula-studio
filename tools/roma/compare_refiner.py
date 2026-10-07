"""Manually compare native VGG features and each RoMa refiner with the pinned FP32 reference."""

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
    from romav2.features import FineFeatures
    from romav2.refiner import Refiners

    torch.set_num_threads(8)
    torch.set_float32_matmul_precision("highest")
    vgg = FineFeatures(FineFeatures.Cfg()).eval().float()
    refiners = Refiners(Refiners.Cfg()).eval().float()
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    for model, prefix in [(vgg, "refiner_features."), (refiners, "refiners.")]:
        model.load_state_dict({k[len(prefix):]: v for k, v in checkpoint.items() if k.startswith(prefix)}, strict=True)
    del checkpoint
    h, w = 32, 48
    features, outputs = [], {}
    with torch.inference_mode(), patch("torch.autocast", lambda *a, **kw: contextlib.nullcontext()):
        for view in range(2):
            y, x, c = np.indices((h, w, 3))
            rgb = ((17 * x + 31 * y + 73 * c + 43 * view) % 256).astype(np.float32) / 255
            feats = vgg(torch.from_numpy(rgb).permute(2, 0, 1)[None])
            features.append(feats)
            for scale, value in feats.items():
                outputs[f"vgg_{view}_{scale}"] = value
        for scale in [4, 2, 1]:
            fh, fw, cc = h // scale, w // scale, 1 if scale == 4 else 4
            y, x = np.indices((fh, fw), dtype=np.float32)
            warp = np.stack((2 * (x + 0.5) / fw - 1 + 0.083,
                             2 * (y + 0.5) / fh - 1 - 0.047), -1)
            warp[0, 0, 0] = -1.05
            conf = np.full((fh, fw, cc), 0.25, dtype=np.float32)
            if cc == 4:
                conf[..., 1:] = [1.2, -0.05, 0.8]
            pred = refiners[str(scale)](f_A=features[0][scale], f_B=features[1][scale],
                                       prev_warp=torch.from_numpy(warp)[None],
                                       prev_confidence=torch.from_numpy(conf)[None],
                                       scale_factor=torch.tensor([w / 512, h / 512]))
            for name, value in pred.items():
                outputs[f"refiner_{scale}_{name}"] = value
    failed = False
    for name, value in outputs.items():
        expected = value.numpy().reshape(-1).astype(np.float64)
        actual = np.fromfile(Path(args.native_dump) / f"{name}.f32", dtype=np.float32).astype(np.float64)
        if actual.shape != expected.shape:
            raise ValueError(f"{name}: shape {actual.shape}, expected {expected.shape}")
        relative_l2 = np.linalg.norm(actual - expected) / np.linalg.norm(expected)
        maximum = np.max(np.abs(actual - expected))
        passed = bool(np.isfinite(actual).all() and relative_l2 < 2e-4)
        failed |= not passed
        print(f"{'PASS' if passed else 'FAIL'} {name}: relative L2 {relative_l2:.8g}, max absolute {maximum:.8g}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
