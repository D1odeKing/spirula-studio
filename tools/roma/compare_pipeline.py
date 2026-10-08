"""Compare every native RoMa stage and the public session with pinned upstream FP32 inference."""

import argparse
import contextlib
import json
import sys
from pathlib import Path
from unittest.mock import patch

import numpy as np
import torch
import torch.nn.functional as F


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--roma-source", required=True)
    parser.add_argument("--dinov3-source", required=True)
    parser.add_argument("--native-dump", required=True)
    parser.add_argument("--profile-preset", choices=["turbo", "fast", "base", "precise"])
    parser.add_argument("--isolate-matcher", action="store_true")
    parser.add_argument("--isolate-refiners", action="store_true")
    parser.add_argument("--profile-pattern", choices=["textured", "aliasing"], default="textured")
    parser.add_argument("--profile-inputs", action="store_true", help="read the exact decoded native profile inputs")
    parser.add_argument("--save-reference", action="store_true", help="save final reference maps for controlled geometry checks")
    args = parser.parse_args()
    sys.path.insert(0, str(Path(args.roma_source) / "src"))
    sys.path.insert(0, args.dinov3_source)
    from dinov3.hub.backbones import dinov3_vitl16
    with patch("torch.cuda.is_available", lambda: False), patch("torch.backends.mps.is_available", lambda: False):
        from romav2.romav2 import RoMaV2
        from romav2.features import Descriptor
        from romav2.matcher import Matcher
        import romav2.matcher as matcher_module

    torch.set_num_threads(8)
    torch.set_float32_matmul_precision("highest")
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    with patch("torch.hub.load", lambda *a, **kw: dinov3_vitl16(pretrained=False)), \
            patch("torch.hub.load_state_dict_from_url", lambda *a, **kw: checkpoint):
        cfg = RoMaV2.Cfg(descriptor=Descriptor.Cfg(enable_amp=False), matcher=Matcher.Cfg(enable_amp=False))
        model = RoMaV2(cfg).eval().float()
    del checkpoint
    model.f.rope_embed.dtype = torch.float32
    model.matcher.mv_vit.rope_embed.dtype = torch.float32

    def image(w, h, view):
        y, x, c = np.indices((h, w, 3))
        rgb = ((17 * x + 31 * y + 73 * c + 43 * view) % 256).astype(np.float32) / 255
        return torch.from_numpy(rgb).permute(2, 0, 1)[None]

    a, b = image(53, 37, 0), image(61, 43, 1)
    failed = False
    worst = 0.0
    checked = 0
    measurements = []

    def compare(name, value, staged=False):
        nonlocal failed, worst, checked
        expected = value.detach().numpy().reshape(-1).astype(np.float64)
        path = Path(args.native_dump) / f"{name}.npy"
        if staged and not path.exists():
            return
        actual = (np.load(path).reshape(-1) if staged else
                  np.fromfile(Path(args.native_dump) / f"{name}.f32", dtype=np.float32)).astype(np.float64)
        if actual.shape != expected.shape:
            raise ValueError(f"{name}: shape {actual.shape}, expected {expected.shape}")
        relative_l2 = np.linalg.norm(actual - expected) / max(np.linalg.norm(expected), 1e-12)
        maximum = np.max(np.abs(actual - expected))
        passed = bool(np.isfinite(actual).all() and relative_l2 < 2e-4)
        failed |= not passed
        worst = max(worst, relative_l2)
        checked += 1
        measurements.append(dict(name=name, passed=passed, relative_l2=float(relative_l2), maximum_absolute=float(maximum)))
        print(f"{'PASS' if passed else 'FAIL'} {name}: relative L2 {relative_l2:.8g}, max absolute {maximum:.8g}")

    with torch.inference_mode(), patch("torch.autocast", lambda *a, **kw: contextlib.nullcontext()):
        if args.profile_preset:
            model.apply_setting(args.profile_preset)
            if args.profile_inputs:
                root = Path(args.native_dump)
                aw, ah, bw, bh = map(int, (root / "input_dimensions.txt").read_text().split())
                rgb_a = np.fromfile(root / "input_a.f32", dtype=np.float32).reshape(ah, aw, 3)
                rgb_b = np.fromfile(root / "input_b.f32", dtype=np.float32).reshape(bh, bw, 3)
            else:
                h, w = 960, 1280
                y, x, c = np.indices((h, w, 3))
                xa, xb, yy = (x, x + 11, y) if args.profile_pattern == "aliasing" else (x // 8, (x + 11) // 8, y // 8)
                rgb_a = ((17 * xa + 31 * yy + 73 * c) % 256).astype(np.float32) / 255
                rgb_b = ((17 * xb + 31 * yy + 73 * c) % 256).astype(np.float32) / 255
            calls = {"descriptor": 0, 4: 0, 2: 0, 1: 0}
            directions = 2 if model.bidirectional else 1

            def descriptor_hook(module, inputs, output):
                view = "b" if calls["descriptor"] else "a"
                calls["descriptor"] += 1
                for tap, value in enumerate(output):
                    compare(f"descriptor_{view}_{tap}", value, staged=True)

            def matcher_hook(module, inputs, output):
                for direction in ["AB", "BA"][:directions]:
                    for name in ["warp", "confidence"]:
                        compare(f"matcher_{direction}_{name}", output[f"{name}_{direction}"], staged=True)

            def refiner_hook(scale):
                def capture(module, inputs, output):
                    index = calls[scale]
                    stage = "high" if index >= directions else "low"
                    direction = "BA" if index % directions else "AB"
                    calls[scale] += 1
                    for name, value in output.items():
                        compare(f"{stage}_{scale}{direction}_{name}", value, staged=True)
                return capture

            a = torch.from_numpy(rgb_a).permute(2, 0, 1)[None]
            b = torch.from_numpy(rgb_b).permute(2, 0, 1)[None]
            for stage, size in [("low", (model.H_lr, model.W_lr))] + (
                    [("high", (model.H_hr, model.W_hr))] if model.H_hr else []):
                for view, rgb in [("a", a), ("b", b)]:
                    compare(f"{stage}_{view}", F.interpolate(rgb, size=size, mode="bicubic", align_corners=False,
                                                            antialias=True).permute(0, 2, 3, 1), staged=True)
            hooks = [model.f.register_forward_hook(descriptor_hook), model.matcher.register_forward_hook(matcher_hook)]
            hooks += [model.refiners[str(scale)].register_forward_hook(refiner_hook(scale)) for scale in [4, 2, 1]]
            hooks += [model.matcher.mv_vit.register_forward_hook(
                lambda module, inputs, output: compare("matcher_mv", output["x_norm_patchtokens"], staged=True))]
            hooks += [model.matcher.mv_vit.projector.register_forward_hook(
                lambda module, inputs, output: compare("matcher_projected", output, staged=True))]
            def block_hook(index):
                return lambda module, inputs, output: compare(f"matcher_block_{index}", output, staged=True)
            hooks += [block.register_forward_hook(block_hook(index))
                      for index, block in enumerate(model.matcher.mv_vit.blocks)]
            original_embedding = matcher_module._compute_match_embeddings
            embedding_calls = 0

            def capture_embedding(**kwargs):
                nonlocal embedding_calls
                output = original_embedding(**kwargs)
                compare("embedding_" + ("BA" if embedding_calls else "AB"), output[2], staged=True)
                embedding_calls += 1
                return output

            try:
                with patch.object(matcher_module, "_compute_match_embeddings", capture_embedding):
                    if args.isolate_matcher:
                        features = [[torch.from_numpy(np.load(Path(args.native_dump) / f"descriptor_{view}_{tap}.npy"))[None]
                                     for tap in range(2)] for view in ["a", "b"]]
                        model.matcher(*features, a, b, model.bidirectional)
                        print(f"Compared {checked} tensors; worst relative L2 {worst:.8g}")
                        return int(failed)
                    if args.isolate_refiners:
                        def read_map(name):
                            return torch.from_numpy(np.load(Path(args.native_dump) / f"{name}.npy"))[None]

                        for stage in ["low", "high"][:2 if model.H_hr else 1]:
                            features = [model.refiner_features(read_map(f"{stage}_{view}").permute(0, 3, 1, 2))
                                        for view in ["a", "b"]]
                            for scale in [4, 2, 1]:
                                for index, direction in enumerate(["AB", "BA"][:directions]):
                                    previous = (f"matcher_{direction}" if stage == "low" else f"low_1{direction}") if scale == 4 else f"{stage}_{scale * 2}{direction}"
                                    warp = read_map(previous + "_warp")
                                    confidence = read_map(previous + "_confidence")
                                    if stage == "high" and scale == 4:
                                        confidence[..., 1:] = 0
                                    h, w = features[index][scale].shape[1:3]
                                    def resize(value):
                                        return F.interpolate(value.permute(0, 3, 1, 2), (h, w), mode="bilinear",
                                                             align_corners=False).permute(0, 2, 3, 1)
                                    model.refiners[str(scale)](f_A=features[index][scale], f_B=features[1 - index][scale],
                                                              prev_warp=resize(warp), prev_confidence=resize(confidence),
                                                              scale_factor=torch.tensor([w * scale / 512, h * scale / 512]))
                        print(f"Compared {checked} tensors; worst relative L2 {worst:.8g}")
                        return int(failed)
                    pred = model.match(a, b)
            finally:
                for hook in hooks:
                    hook.remove()
            for direction in ["AB", "BA"][:2 if model.bidirectional else 1]:
                compare(f"profile_{direction}_warp", pred[f"warp_{direction}"])
                compare(f"profile_{direction}_overlap", pred[f"overlap_{direction}"])
                warp = pred[f"warp_{direction}"].numpy()[0]
                native = np.fromfile(Path(args.native_dump) / f"profile_{direction}_warp.f32", dtype=np.float32).reshape(warp.shape)
                pixel_error = np.linalg.norm((native - warp) * np.array([warp.shape[1], warp.shape[0]]) / 2, axis=-1)
                overlap = pred[f"overlap_{direction}"].numpy().reshape(warp.shape[:2])
                accepted = pixel_error[overlap >= 0.5]
                print(f"{direction} warp error in output pixels: RMS {np.sqrt(np.mean(pixel_error ** 2)):.6g}; "
                      f"overlap>=0.5 pixels {accepted.size}; " +
                      (f"median {np.median(accepted):.6g}, p99 {np.quantile(accepted, 0.99):.6g}, max {accepted.max():.6g}"
                       if accepted.size else "no accepted matches"))
                precision = pred[f"precision_{direction}"]
                packed = torch.stack((precision[..., 0, 0], precision[..., 0, 1], precision[..., 1, 1]), -1)
                compare(f"profile_{direction}_precision", packed)
                if args.save_reference:
                    for name, value in [("warp", pred[f"warp_{direction}"]),
                                        ("overlap", pred[f"overlap_{direction}"]), ("precision", packed)]:
                        value.detach().numpy().astype(np.float32).tofile(
                            Path(args.native_dump) / f"reference_profile_{direction}_{name}.f32")
                if args.profile_inputs:
                    dimensions = (bw, bh) if direction == "AB" else (aw, ah)
                    original_error = np.linalg.norm((native - warp) * np.array(dimensions) / 2, axis=-1)
                    accepted_error = original_error[overlap >= 0.5]
                    measurements.append(dict(name=f"{direction}_original_pixel_error", accepted=int(accepted_error.size),
                                             rms=float(np.sqrt(np.mean(original_error ** 2))),
                                             median=float(np.median(accepted_error)) if accepted_error.size else None,
                                             p99=float(np.quantile(accepted_error, 0.99)) if accepted_error.size else None,
                                             maximum=float(accepted_error.max()) if accepted_error.size else None))
            (Path(args.native_dump) / "comparison.json").write_text(
                json.dumps(dict(gate=2e-4, passed=not failed, worst_relative_l2=worst,
                                checked=checked, measurements=measurements), indent=2))
            print(f"Compared {checked} tensors; worst relative L2 {worst:.8g}")
            return int(failed)
        for test_case in range(3):
            prefix = f"case{test_case}_"
            model.bidirectional = test_case < 2
            outputs = {}
            calls = {scale: 0 for scale in [4, 2, 1]}
            directions = 2 if model.bidirectional else 1

            def matcher_hook(module, inputs, pred):
                for direction in ["AB", "BA"][:directions]:
                    for name in ["warp", "confidence"]:
                        outputs[f"matcher_{direction}_{name}"] = pred[f"{name}_{direction}"].clone()

            def refiner_hook(scale):
                def capture(module, inputs, pred):
                    index = calls[scale]
                    stage = "high" if index >= directions else "low"
                    direction = "BA" if index % directions else "AB"
                    calls[scale] += 1
                    for name, value in pred.items():
                        outputs[f"{stage}_{scale}{direction}_{name}"] = value.clone()
                return capture

            hooks = [model.matcher.register_forward_hook(matcher_hook)]
            hooks += [model.refiners[str(scale)].register_forward_hook(refiner_hook(scale)) for scale in [4, 2, 1]]
            inputs = {}
            for stage, size in [("low", (32, 48))] + (
                    [("high", (16, 24) if test_case == 2 else (48, 64))] if test_case else []):
                for name, rgb in [("a", a), ("b", b)]:
                    resized = F.interpolate(rgb, size=size, mode="bicubic", align_corners=False, antialias=True)
                    inputs[f"{stage}_{name}"] = resized
                    compare(prefix + f"{stage}_{name}", resized.permute(0, 2, 3, 1))
            try:
                pred = model(inputs["low_a"], inputs["low_b"], inputs.get("high_a"), inputs.get("high_b"))
            finally:
                for hook in hooks:
                    hook.remove()
            for name, value in outputs.items():
                compare(prefix + name, value)
            if test_case == 1:
                for direction in ["AB", "BA"]:
                    confidence = pred[f"confidence_{direction}"]
                    compare(f"session_{direction}_warp", pred[f"warp_{direction}"])
                    compare(f"session_{direction}_overlap", confidence[..., :1].sigmoid())
                    compare(f"session_{direction}_precision", confidence[..., 1:])
    print(f"Compared {checked} tensors; worst relative L2 {worst:.8g}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
