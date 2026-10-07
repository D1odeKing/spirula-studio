"""Manually compare the native FP32 descriptor to the pinned DINOv3 source."""

import argparse
import sys
from pathlib import Path

import numpy as np
import torch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--dinov3-source", required=True)
    parser.add_argument("--native-dump", required=True)
    parser.add_argument("--height", type=int, default=32)
    parser.add_argument("--width", type=int, default=48)
    args = parser.parse_args()
    sys.path.insert(0, args.dinov3_source)
    from dinov3.hub.backbones import dinov3_vitl16

    torch.set_num_threads(8)
    torch.set_float32_matmul_precision("highest")
    model = dinov3_vitl16(pretrained=False).eval().float()
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    model.load_state_dict({k[2:]: v for k, v in checkpoint.items() if k.startswith("f.")}, strict=True)
    del checkpoint
    y, x, c = np.indices((args.height, args.width, 3))
    rgb = ((17 * x + 31 * y + 73 * c) % 256).astype(np.float32) / 255
    image = torch.from_numpy(rgb).permute(2, 0, 1)[None]
    mean = torch.tensor([0.485, 0.456, 0.406])[None, :, None, None]
    std = torch.tensor([0.229, 0.224, 0.225])[None, :, None, None]
    with torch.inference_mode():
        outputs = model.get_intermediate_layers((image - mean) / std, n=[11, 17])
    failed = False
    for tap, reference in enumerate(outputs):
        expected = reference.numpy().reshape(-1).astype(np.float64)
        actual = np.fromfile(Path(args.native_dump) / f"descriptor_{tap}.f32", dtype=np.float32)
        if actual.shape != expected.shape:
            raise ValueError(f"tap {tap}: native shape {actual.shape}, expected {expected.shape}")
        difference = actual.astype(np.float64) - expected
        relative_l2 = np.linalg.norm(difference) / np.linalg.norm(expected)
        cosine = np.dot(actual.astype(np.float64), expected) / (
            np.linalg.norm(actual.astype(np.float64)) * np.linalg.norm(expected)
        )
        maximum = np.max(np.abs(difference))
        passed = bool(np.isfinite(actual).all() and relative_l2 < 2e-4 and cosine > 0.999999)
        failed |= not passed
        print(f"{'PASS' if passed else 'FAIL'} tap {tap}: relative L2 {relative_l2:.8g}, "
              f"cosine {cosine:.10g}, max absolute {maximum:.8g}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
