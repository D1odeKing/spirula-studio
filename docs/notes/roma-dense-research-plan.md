# Plan to finish the RoMa dense research

Planning record: 2026-10-05. This document specifies implementation and
validation work; it does not certify completion of the release gates below.

The basis is [the research and validation log](roma-dense-validation.md),
[AGENTS.md](../../AGENTS.md), [dense usage](../dense.md), the
[dense subsystem](../../src/dense/README.md), the
[RoMa subsystem](../../src/roma/README.md), and the repository's
[build](../build.md) and [testing](../testing.md) requirements.

## Starting point

The research log contains successive investigations. Earlier descriptions of
rectified-only matching, provisional pairwise previews, and same-cell fusion
describe historical behavior. The current working tree already implements
source matching, source-camera geometry, reference completion, filtered live
checkpoints, parallel reference processing, disk spill, neighboring-cell
fusion, per-pair prediction identities, content verification, and immutable
output generations. Keep these implementations and extend their existing
tests. Do not replace them with another reconstruction pipeline.

The principal remaining work is acceptance and any fixes it identifies:

- Complete native/reference inference investigation at production resolutions.
- Explain how correspondence differences affect accepted geometry.
- Complete ordinary-photo, fisheye, panorama, and full mixed-camera runs with
  controlled comparisons and the production mask/support policy.
- Measure complete cold and warm workloads, including final fusion,
  verification, publication, preview overhead, and memory.
- Finish device, backend, build-mode, download, GUI, and training validation.
- Reconcile documentation with the latest results and preserve unresolved
  limitations explicitly.

Existing ignored diagnostics contain forty comparisons: two checkpoints,
four presets, and five input types. Nine pass the `2e-4` numerical gate and
thirty-one fail; all forty native runs completed. On the official real-photo
Precise case, all twenty-eight isolated resize/refiner comparisons pass with
worst relative L2 about `1.695e-5`. These results justify investigating cascade
sensitivity, but establish neither full numerical parity nor scene quality.
The validation log still describes the matrix as running and needs an update
during implementation.

## Requirements to retain

- Use the existing native RoMa session and reusable `nn/` operations. The
  sparse LoMa/LightGlue matcher cannot supply RoMa's dense warp, overlap, and
  precision fields. Python references remain manual development tools, outside
  the application, native tests, and build dependencies.
- Keep rectified matching as the default, including old settings. Source
  matching remains opt-in and preserves inactive rectification settings.
- Keep source setup at reference fraction `0.8`, three neighbors, `10,000`
  samples per reference, sampling seed zero, and `0.08` original-image pixels.
  These are editable workload/quality choices, not hardware limits. Sampling
  count zero means all eligible grid locations.
- Preserve the selected preset, dimensions, checkpoint, precision, masks,
  genuine confidence rejection, and three distinct source images. Resource
  adaptation must not silently weaken these choices.
- Preserve camera poses and calibrations. Use `data/CameraMath` for host camera
  projection and bearings; retain the existing device camera implementation.
  Introduce no third camera implementation or second dataset parser.
- Preserve full-cloud checkpoints and rendering without a fixed point cap.
  Budget caches, queues, workers, and partitions from the selected hardware.
  Model alignment, tensor-index ranges, and actual driver limits still apply.
- Keep the official checkpoint as the default. Compare the older reference
  checkpoint through the existing local-checkpoint mechanism.
- Preserve existing uncommitted work, source datasets, and successful results.
  Use owned, ignored validation directories. Keep weights, private paths,
  capture names, and diagnostic artifacts out of committed documentation.

## 1. Establish a reproducible acceptance baseline

Create one case record per validation workload containing input-content
digests, camera/calibration identity, checkpoint digest, resolved configuration,
source revision, selected device, resolved precision, and output generation.
Keep historical measurements separate from measurements of the current code.

Use isolated copies for writes. Prepare a posed analytic fixture, subsets
containing ordinary photos, fisheye images, and panoramas, a mixed subset, and
the complete mixed scene. Select subsets with connected overlap sufficient
for the requested support. A convenient camera-type sample with disconnected
neighbors is unsuitable for assessing reconstruction density.

Audit the existing source workflow against the research requirements:
deterministic scale-normalized reference coverage; sparse-track/affinity ranking;
directed job semantics; genuine reverse fields; support validation before model
loading; and streaming enumeration without an image-by-image matrix.
Retain sequential, explicit, exhaustive, and rectified behavior. Address any
remaining large-list allocations without changing the requested pair graph.

**Completion evidence:** reproducible case records, unchanged input digests,
and a baseline table distinguishing implemented behavior, tested behavior,
failed comparisons, and tests still pending. Reuse a valid existing run rather
than restarting it solely to obtain a new diagnostic directory.

## 2. Investigate inference discrepancies and measure geometry effects

Keep the forty-case matrix: both documented checkpoint digests; Turbo, Fast,
Base, and Precise; textured, aliasing, real-photo, fisheye, and panorama pairs.
Use the exact float RGB decoded by the native profile tool for both inference
implementations. Fix checkpoint bytes, precision, dimensions, direction,
normalization, and half-pixel/`align_corners=false` conventions.

For each failing case, identify the first divergent stage using existing
descriptor, matcher, refiner, and pipeline dumps. Repeat isolated comparisons
with identical preceding inputs to distinguish an operator error from
accumulated sensitivity. Extend shared operators only when evidence identifies
an error; preserve LoMa's established resize behavior and keep RoMa policy in
`src/roma/`. Retain the `2e-4` gate and record field-specific errors rather than
renaming failures as passes.

Add a development diagnostic that reads saved native and reference fields
and sends both through the existing `dense::Reconstruction` core with the
same parsed cameras, sampling seed, masks, confidence rejection, cycle policy,
parallax, depth consistency, and support. The diagnostic must use the shared
camera math and point optimizer. It must not reimplement triangulation or
masquerade reference fields as production prediction-cache entries.

Start with two-image controls, explicitly labeled as two-image tests. Then
compare connected multi-neighbor cases with masks and three-image support.
Report common accepted reference samples, native-only/reference-only accepted
samples, rejection reasons, support, source-pixel residuals, and spatial
differences in reconstructed points. Low residual alone cannot establish a
correct correspondence, so include known surfaces and matching camera views.

Evaluate mixed inference separately against native FP32 using the same scene
jobs. Record coverage, residual tails, surface differences, masked leakage,
memory, and time. Do not infer geometric equivalence from similar point counts
or cached/uncached equality within one precision mode.

**Completion evidence:** a complete numerical report, diagnosis of failures,
geometry-effect measurements, and remaining discrepancies stated explicitly.
Numerical parity remains an open gate for any case that still fails it.

## 3. Close calibrated geometry and live-preview acceptance

Extend the existing geometry and camera tests only where coverage is missing:
source-ray intersections, half-pixel conversions, precision transformation into
the observation pixel frame, projection derivatives, lens boundaries, valid
rear-facing fisheye rays, panorama seams/poles, cropped panoramas, low parallax,
mismatches, masked samples, large coordinates, and source-frame export.
Source cheirality and depth consistency must use the observed ray; a universal
positive camera-Z test is invalid for spherical cameras. Keep the analytic
pinhole path and rectified pixel tolerance unchanged.

Verify that reference completion waits for all scheduled neighbors and uses
the same deterministic 15% coverage/85% weighted selection in memory and on
disk. Recheck every observation after robust refinement and enforce distinct
original-image support before checkpoint publication. Audit periodic mask
boundary and local-normal neighborhoods as well as warp/cycle interpolation.

Validate the live point file against the corresponding filtered reconstruction
records and compare fixed viewpoints with the completed PLY. The processing
viewport must identify filtered points awaiting fusion and then switch to the
final generation. Check exact coordinate decoding, camera metadata, legacy
snapshot readers, hide/show, final refresh, and clouds above eight million
points. Loading and uploading follow measured cost and available memory;
memory pressure may pause display visibly while processing continues.

Validate neighboring-cell fusion on real surfaces as well as analytic fixtures.
Check halo width against the compatibility radius, deterministic boundary
ownership, discontinuity preservation, oversized cells, and bounded merge
chains. Measure whether final fused positions retain the required geometric
quality. If fusion introduces invalid shifts, preserve or recheck necessary
support information through the existing core before publication.

**Completion evidence:** passing targeted regressions, identical
serial/parallel/spilled deterministic outputs, and completed camera-type scenes
whose filtered and final clouds have documented coverage, support, residuals,
and visual artifact checks. Camera round trips alone are insufficient.

## 4. Complete the full scene and optimize measured bottlenecks

Run two forms of comparison. First control decoded pixels, checkpoint, jobs,
sampling, masks, and filters. Then run the production policy with the
capture's masks and three-image support enabled. A reference pipeline's ignored
masks, confidence clamping, unenforced track minimum, and preview reservoir
must not become native defaults.

Complete the full mixed-camera scene, followed by a warm run. Separate a run
with no prediction cache, a run with verified prediction reuse, and a run
reusing the final generation. Record source images/references, directed jobs,
unique image pairs, actual inference calls, feature/prediction cache hits,
all processing stages, total wall time, output checksum, and point quality.
Use matched camera viewpoints to inspect stretched sheets, floaters, seams,
missing surfaces, and masked-area leakage. Report coverage by camera type.

Measure peak process working set/commit, tracked Vulkan buffers, scratch,
and driver-resident device/shared memory as separate quantities. Include
reference worker children when measuring process memory; sampling a launcher
alone is invalid. Label diagnostic synchronization and competing workloads.
Run timing repetitions without compilation or unrelated GPU work before
claiming a speedup. Measure preview-enabled and preview-disabled overhead.

Prioritize fixes according to those measurements: unnecessary face expansion,
descriptor recomputation, decode waits, prediction I/O, reference grouping,
fusion, hashing, or preview uploads. Reuse the existing prefetch, feature LRU,
prediction cache, spill, and worker infrastructure. Keep GPU work overlapped
with CPU preparation/refinement through bounded queues and backpressure.

All automatic budgets derive from CPU count, available host RAM, selected
device heaps, live allocations, scratch requirements, and optional driver
budgets. Integrated devices must account for shared memory pressure. Evict
features, reduce concurrency, or spill records before rejecting optional cache
allocations. If the selected model itself cannot fit, give an actionable error
without silently lowering resolution, precision, sampling, or support.

**Completion evidence:** completed cold/warm scene results, complete timings
and memory accounting, documented preview cost, and quality preserved by each
optimization. Optimize further after a correct complete run; promise no
unmeasured whole-dataset speedup.

## 5. Finish cache, publication, cancellation, and cleanup validation

Keep prediction identity separate from reconstruction identity. Verify image
content/preprocessing, checkpoint, precision, dimensions, direction, and
inference revision per pair. Pose, mask, sampling, support, and fusion changes
must rebuild geometry while reusing unchanged source predictions where valid.
Rectified preprocessing identities must still include relevant calibration.

Exercise same-size/same-timestamp hidden edits, corrupt predictions, changed
inputs during processing, verified legacy cache promotion, and resume. Hash
actual contents before successful result reuse and before publication; GUI
metadata is only a provisional freshness probe.

Inject cancellation during loading, inference, sampling, optimization, external
sorting, fusion, verification, and publication. Test spill cleanup even when
streams have already closed. Preserve verified predictions and the last
successful cloud on interruption.

Keep immutable PLY/manifest generations and the atomic current record. Check
planner, preview, CLI training, GUI training, batch handoff, relative paths,
legacy aliases, concurrent writers, first-publication interruption, and pinned
generation lifetime. Cleanup must validate application ownership and reject
redirected roots, parents, and nested junctions/symlinks. Preserve published,
pinned, and unmarked directories.

**Completion evidence:** fault-injection checks on current builds, targeted
invalidation counts, unchanged preceding-generation checksums, and no abandoned
owned temporary records after successful completion.

## 6. Validate devices, builds, downloads, GUI, and training

Use capability queries, never GPU model names, to select kernels and automatic
precision. Exercise available NVIDIA and AMD Vulkan devices, integrated/shared
memory, and cooperative-matrix-disabled fallback. Run all presets that fit
the requested hardware and test eviction, low budgets, allocation failure,
cancellation, and cached/uncached equality within each precision mode.
Record unavailable hardware rather than claiming universal physical testing.

Build through `build_develop.bat`/`build_develop.bash`. Check CUDA and Vulkan
trainer builds, inference enabled and disabled, GUI and headless modes, and
available non-Windows platforms. The dense host geometry remains portable;
RoMa inference remains the optional Vulkan subsystem under `SS_BUILD_SAM`.
Any trainer kernel change needs both implementations and a parity test.
Keep `SS_ENABLE_PATENTED=OFF`. Do not relink an executable used by an active run.

Exercise official-model consent with both licenses, cancellation before/during
download, checksum failure preserving a valid checkpoint, and successful HTTP
verification through the existing model cache/downloader. Local-file download
tests establish byte verification but do not replace the HTTP acceptance case.

Use the repository's GUI automation and shared viewer. Verify setting restore,
source setup, preset round trips, planner reuse/rerun, cancellation, all-cloud
preview, and training handoff. Hover help must explain each option and its
effect on training initialization through existing `ui::` wrappers, with all
thirteen translations and font coverage. Training must pin the verified dense
generation and retain explicit seed/mask-policy precedence.

Run a short public-scene training check on each trainer backend, as required by
`docs/testing.md`, plus the dense-seed/mask-policy handoff fixture. A two-step
run checks integration and checkpoint saving; it does not prove convergence.

**Completion evidence:** device/build-mode matrix, download checks, inspected
GUI results, training checkpoints, and explicit unavailable-test entries.

## Delivery order and documentation

1. Capture the current baseline and connected acceptance fixtures.
2. Investigate numerical failures and compare geometry through the shared core.
3. Close source-camera, filtered-preview, and fusion quality checks.
4. Complete the full production scene and cold/warm measurements; fix measured
   performance problems without weakening quality.
5. Finish fault injection, device/build modes, download, GUI, and training gates.
6. Update the research log, dense usage, and subsystem READMEs with settings,
   revisions, evidence, results, failures, and remaining limitations.

Build and run relevant checks after each substantive change. Once they pass,
repeat them only for new changes or unresolved concerns. Final checks include
comment length/reference, `SS_` prefix, `SS_FILE`, translations, font coverage,
private paths, and whitespace. Keep comments within the AGENTS budgets and put
architecture and measurement narratives in documentation.

Delivery requires concrete evidence for complete scene processing and each
available release gate. A finite-output matcher, a small analytic cloud, a
calibration round trip, or similar point counts cannot close real-scene
acceptance. Any unresolved numerical discrepancy or unavailable platform test
stays visible in the final record.
