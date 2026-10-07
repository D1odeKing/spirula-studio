# Native RoMa v2

RoMa v2 inference over `nn/`, with native C++ checkpoint loading and Vulkan
operations. Built under `SS_BUILD_SAM`, independently of the trainer backend.
The application and build require neither Python nor PyTorch.

The port follows upstream RoMa revision
`95c9968145c8906b7b59383258e9f73b02853d89`, and its DINOv3 reference revision
`adc254450203739c8149213a7a69d8d905b4fcfa`.
The official `v2.0.1/romav2.0.1.pt` checkpoint has 907 tensors and SHA-256
`1557dec0d21b62366465f7ff4d5fdf228cc695d0582e196ad2b80e05230828b7`.
Checkpoint weights are not distributed with this repository. Upstream RoMa's
code is MIT except for DINOv3, which has its own license:
<https://github.com/Parskatt/RoMaV2#license>.

## Implementation status

The native checkpoint reader validates a fixed container/tensor pickle
whitelist and streams one tensor storage at a time. The model loader validates
the complete RoMa architecture before uploading and bounds host staging to
roughly 64 MiB plus the current tensor. Device weights use chunked allocations.
The numerical reference path stores weights and activations as FP32.
Mixed mode stages GEMM and ordinary convolution weights through `nn::WeightStore`
as FP16, keeping normalization, biases, depthwise weights, activations and
accumulation FP32. Cooperative attention is permitted in mixed mode and gated
by `nn/`'s device probe. `Session::load` keeps FP32 as its API default;
dataset processing passes its explicit precision policy. Auto resolves to mixed
on compatible cooperative-matrix devices and FP32 elsewhere. Explicit mixed
mode remains usable with the portable packed-weight kernels. Reloading weights
clears the feature cache, so different precisions cannot share descriptors.

`model/Descriptor.cpp` implements DINOv3-L/16's two normalized feature maps at
blocks 11 and 17. Its five prefix tokens remain in attention, its key bias mask
is applied at loading, and its positional encoding uses split-half rotations.
The multiview transformer and DPT coarse matcher, VGG19-BN feature pyramid,
and refiners at scales 4, 2, and 1 are implemented. `Roma.h` exposes a session
for dense image-pair inference, including optional high-resolution refinement
and both directions. Images can have different original dimensions. Returned
warps use normalized target coordinates with `align_corners=false`; overlap
is a probability, and precision is `(xx,xy,yy)` in matching-grid pixel units.
LoMa and RoMa share `nn/io/Resize.h`'s antialiased bicubic float RGB resize;
RoMa selects FP32 coordinate/weight arithmetic, while LoMa keeps its existing
Pillow behavior. ImageNet normalization subtracts before dividing.
High-resolution refinement keeps the preceding overlap and resets precision.
The FP32 path disables reduced-precision cooperative attention per call.

Session presets match upstream Turbo, Fast, Base, and Precise. Dimensions,
bidirectionality, and optional overlap saturation remain independently editable.
Custom low-resolution sides
must be multiples of 16 and high-resolution sides multiples of 4. Either both
high-resolution sides are zero or both are positive. The session preflights a
conservative scratch reservation against an explicit device-memory budget or
80% of the selected device's reported memory. It never reduces resolution to
fit. Progress callbacks can throw to cancel; unloading returns session-owned
device allocations. One session runs on one thread at a time.

`Session::matchCached` reuses resized RGB inputs and DINO descriptor maps by
session-local view ID. A view ID must keep the same RGB content until
`clearFeatureCache` or weight reload. Dimensions are checked on lookup;
changing the matching grid replaces that view's cached features. Direction
and overlap saturation do not affect the descriptors. The automatic feature
budget follows selected-device headroom after weights, scratch, and other
inference allocations. Optional driver budget reporting adjusts it to memory
pressure; the device's allocation-count limit constrains cache entries.
There is no fixed cache byte or entry cap. `setFeatureCacheBudget(0)` disables
reuse; an empty budget restores automatic sizing. Eviction and insufficient cache
space fall back to the same selected-precision pipeline. `match` remains uncached for
reference comparisons. Cache statistics and device-byte accounting include
session-owned features; unloading frees them.

Dataset processing uses this cache through `spirula dense` and the optional
GUI step. Dense triangulation and fusion live in `src/dense/`; usage is in
`docs/dense.md`.

## Validation

`torch_checkpoint_test [CHECKPOINT]` runs CPU-only malformed-file and tensor-view
tests; with the official checkpoint it also decodes every tensor.
`roma_descriptor_test CHECKPOINT [DUMP_DIRECTORY]` runs the native FP32
descriptor on a deterministic 32x48 input. The manual reference comparison is:

```text
python tools/roma/compare_descriptor.py --checkpoint CHECKPOINT \
  --dinov3-source PINNED_DINOV3_CHECKOUT --native-dump DUMP_DIRECTORY
```

`roma_matcher_test` compares coarse heads on 2x3 and 3x5 descriptor grids;
`roma_refiner_test` exercises the feature pyramid, all refiners, precision,
and wide addressing. Their manual references are `compare_matcher.py` and
`compare_refiner.py` under `tools/roma/`.

`roma_pipeline_test CHECKPOINT [DUMP_DIRECTORY]` exercises the complete
pipeline on differently sized original images, one/two-pass and one/two-way
inference, up/downscaling, session outputs, cached/uncached prediction equality,
reverse-view reuse, eviction, cancellation recovery, budgeting, and unload.
Compare every dumped intermediate and session output with:

```text
python tools/roma/compare_pipeline.py --checkpoint CHECKPOINT \
  --roma-source PINNED_ROMA_CHECKOUT --dinov3-source PINNED_DINOV3_CHECKOUT \
  --native-dump DUMP_DIRECTORY
```

On NVIDIA Vulkan, all 74 pipeline comparisons pass a relative-L2 gate of
2e-4; after the preprocessing corrections the worst measured error is 5.48e-5.
These small non-square fixtures
verify numerical conventions, not scene reconstruction quality. All manual
comparison scripts are development tools only. Nothing in the application,
the native tests, or the build invokes or imports them.

Full preset comparisons have exposed accumulated numerical sensitivity and
some outputs still exceed the development gate. They are not yet validated
as passing. Findings, corrected conventions, measurements, and remaining
checks are recorded in `docs/notes/roma-dense-validation.md`. Mixed arithmetic
has separate scene and resource validation and is not claimed as FP32 parity.
The dataset processing workflow is implemented through
`spirula dense` and the optional GUI step; usage is in `docs/dense.md`.

`roma_profile_test CHECKPOINT [PRESET]` measures full preset inference on a
synthetic pair and reports elapsed time and memory. The FP32 Precise preset
holds about 5.2 GB of device memory including 1.7 GB of weights, with a
3.5 GB scratch reservation of which about 2.2 GB is used. Turbo/Fast/Base also
complete at their standard resolutions. The timings include final downloads;
callback intervals reflect command submission and are not GPU-stage timings.

`SS_ROMA_PROFILE_REPEAT=N` repeats matching in one process.
`SS_ROMA_PROFILE_CACHE=off|reference|both` selects uncached matching, one
fixed reference with fresh neighbor IDs, or a repeated cached pair. These
synthetic workloads separate startup from steady-state matching and print
feature-cache statistics. Dumped final predictions use the same format in
all three modes.
`SS_ROMA_PROFILE_PRECISION=auto|float32|mixed` selects weight arithmetic and
prints the resolved mode and device's cooperative-matrix status.

`roma_profile_test CHECKPOINT PRESET DUMP_DIRECTORY IMAGE_A IMAGE_B` uses real
images through native decoding and writes their exact float RGB and dimensions
alongside the final fields. The manual comparison's `--profile-inputs` reads
those inputs, and `--save-reference` retains reference fields for geometry
diagnostics. This keeps decoding differences out of inference comparisons.
The profile also reports peak tracked Vulkan buffers, including host-visible
staging; that allocator number is not physical driver-resident VRAM.
