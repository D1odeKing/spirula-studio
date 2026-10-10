# GPU submit budget

A submission that runs past the driver's watchdog loses the device:
`VK_ERROR_DEVICE_LOST` (-4), and on the inference path a segfault inside the
driver right after it. The watchdog is 2 s under Windows TDR, and 2 s for
amdgpu on Linux 7.0 (`modinfo amdgpu | grep lockup_timeout`; older kernels
used 10 s). Work sized on a discrete GPU crosses that on an integrated one:
a 2-CU RADV iGPU (Ryzen 7000 "Raphael") runs these paths 20-140x slower
than an RTX 5070.

`core/SubmitBudget.h` is the one mechanism. Each caller counts work in its
own units, times its submits, and sizes the next one to about 0.25 s of GPU
time (8x headroom). A fast GPU measures a large rate and keeps its old
batching; only the first submits of a process are smaller.
`SS_SUBMIT_BUDGET_MS` overrides the target -- raise it to reproduce the old
behaviour, lower it to exercise the slicing on a fast GPU (results are
bit-identical except where noted below).

| path | unit | what gets split | measured on the iGPU before |
|---|---|---|---|
| SfM brute-force matcher (`sfm/feature/Matcher.h`) | descriptor words multiplied | pairs per chunk | 64 cross-checked 8192^2 pairs = 2.0 s, one submit |
| SIFT pyramid (`sfm/feature/Sift.h`) | pixels x taps | blur steps per submit | 0.83 s at 3200 px, >2 s at 5000 px |
| SIFT orient / descriptor | keypoints | keypoint ranges | 392 / 222 ms, one dispatch each |
| inference stream (`nn/vk/Stream.cpp`) | FLOPs, per-op estimate | submits by cost; GEMM rows and attention queries/batches past the cap | SAM 3 memory attention ~190 GFLOP in one dispatch |
| meshing cull / occupancy / bisection / color | pairs or points | launch ranges, capped at the old sizes | cull: 0.11 s per launch on an RTX 5070 |
| GPU bundle adjustment (`sfm/ba/Solver.h`) | ~ns of RTX 5070 fp64, per-kernel weights rescaled by a first timed launch | LM iteration at barriers; per-obs, per-chunk and Cholesky-tile launches into ranges | one LM iteration of a 6946-image rig capture: 2.4 s on the RTX 5070 itself |
| training, Vulkan (`backend/vulkan/DispatchBudget.cpp`) | per launch: splat-tile pairs x 4^macro_log2 + pixels for the rasterizer, pixels x levels for the bilateral grid, workgroups otherwise | the launch's longest grid axis into ranges, each its own submit | bilateral-grid backward 5.4 s, raster backward 1.6 s, one dispatch each (4946x3286, 753K splats, Intel UHD) |

The inference stream submits asynchronously, so it brackets every command
buffer with two timestamps and reads them when the ring slot comes round
again; until then it assumes 50 GFLOP/s. Attention cost is weighted 2x,
because flash attention reaches half the efficiency of the GEMMs the rate
is mostly learned from.

Slicing attention by query block can change the key-split decision per
slice, so outputs move by float-reordering noise (a handful of mask-edge
pixels in `spirula geometry`); everything else is bit-identical.

Bundle adjustment weights its kernels as measured on the RTX 5070, and the
first launch of each big kernel on a device is a 1/32-budget range timed
alone, because the ratios do not carry across devices (sfm/ba/README.md
"Watchdog"). It runs on NVIDIA by default (fp64 atomic add) and on anything
with int64 atomics under `--ba-real df`, which is how the iGPU exercises it.

## Training

The training step is one long command buffer between its few host syncs, and
the launches that grow with the input -- the tile rasterizer forward and
backward, the bilateral grid, SSIM and the per-pixel losses -- go through
`backend::vk::dispatch_budgeted`. Each keeps a rate per entry (per entry and
tile size for the bilateral grid, whose selector's arms differ). A launch
predicted past the target runs as ranges of its longest grid axis through
`vkCmdDispatchBase`, so no kernel changed; a pipeline built for that is used
only for split launches, and a launch that fits records exactly as before.
Ranges also flush the open command buffer once the budgeted work in it would
pass the target. The viewer's render is the same forward, so it is covered too.

Timing is on the host, not timestamps: a sampled launch drains the queue and
times each of its ranges alone, and the rate kept is the slowest solid range,
because ranges differ (sky rows, image borders) and the slowest is what a
watchdog sees. A launch predicted at a quarter of the budget or more is
re-timed every 4th call, since its cost per unit moves as loss terms switch
on (the per-pixel loss backward took 2.7x longer from step 2 than at step 1
at 4946x3286); a shorter one every 64th.

An entry's first launch runs whole, which is the one gap: nothing says how
to size it, and splitting it the way bundle adjustment does (a first range of
1/32) built the base-capable pipeline of all ten entries on every device,
8.7 s of a cold first step on an RTX 3070 that never splits again. A key that
has not been timed but whose entry has (the bilateral grid's other tile
sizes) starts from that rate over 4.

Measured on full-resolution bicycle (4946x3286, 753K splats, defaults) on an
Intel UHD iGPU: every step had a 5.4 s submit (bilateral-grid backward, one
dispatch) and a 1.6 s one (raster backward); from step 2 on the longest is
478 ms, p90 305 ms over the submits past 250 ms, and the run's wall time is
unchanged (4:59 -> 4:47 for 30 steps). Step 1 keeps its whole first launches
(5.2 s, 1.2 s). On an RTX 3070 nothing splits: 600 steps at 1237x822 from
3M splats took a median 19.5 s against master's 19.5 s over six alternated
pairs, and the 400-step tile search on the iGPU 2:51 against 2:52. Splitting is
bit-identical for the forward render and within the backward's atomic-ordering
noise elsewhere: `render_parity`, `raster_bwd_parity`, `bilagrid_parity`,
`msloss_parity`, `engine_render_parity` and `engine_train_parity` pass under
`SS_SUBMIT_BUDGET_MS=0.01`, which splits every launch after its entry's first.

`SS_VK_SUBMIT_LOG=<ms>` makes every submit wait for itself and prints the ones
that ran longer, with their kernels -- the number a watchdog compares. That
Intel driver preempts instead of resetting, so it survived 5.4 s; macOS does
not ("Impacting Interactivity", docs/notes/binning-tile-size.md).

Not covered: the CUDA backend, and the per-splat kernels (projection, sort,
optimizer, densify). Those are linear in splat count and have no hotspot; the
largest, the fused projection backward + optimizer, took 45 ms at 753K splats
on that iGPU, so about 300 ms at 5M.
