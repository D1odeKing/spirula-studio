# --hybrid-sift on a7iii_inside

The test runs for `docs/notes/sfm-hybrid-frontend-plan.md`, made on the Dell
(RTX 4060 Laptop, 8 GB, Windows, Vulkan build).

Dataset: `H:\Hershy Site GS\GS runs\a7iii_inside\images`, 678 images: 37
equirectangular 11904x5952 (`360_images_inside_Masked/jpeg`) and 641 A7iii
pinhole 6016x4016 / 4016x6016 (`A7iii_Inside`). No masks.
`manifest.yaml` gives the lens per folder.

Binaries: `bin_base` = `sfm-hybrid-base` (b82f18b8); `bin_work1` =
`sfm-hybrid-work` build 1 (7521cc20).

Each folder has `cmd.txt`, the full `run.log`, `progressive.txt` and, for
hybrid runs, `hybrid.txt`.

## Results

| run | registered | models | points | observations | mean px | median px | total |
|---|---|---|---|---|---|---|---|
| loma_prog (step 0) | 676/678 | 1 | 606,874 | 1,796,609 | 3.015 | 2.611 | 1:04:11 |
| sift_prog (step 0, base) | 645/678 | 1 | 273,451 | 734,543 | 1.454 | 1.151 | ~0:24† |
| hybrid (A) | 676/678 | 1 | 382,112 | 1,014,341 | 1.616 | 1.304 | 0:59:43 |
| hybrid_noguide (B) | 676/678 | 1 | 297,599 | 803,578 | 1.511 | 1.204 | 0:23:47* |

† SIFT extraction, matching and two ladder attempts took about 3 min before the pause
(`sift_prog/run_paused.log`). The rest, 21 min, is `sift_prog/run.log`, resumed over
its own matches.

\* B reused A's LoMa matches, so its total has no matching time.

## Matches per stage

| | pairs | putative | after gate | verified pairs | inliers | stage time |
|---|---|---|---|---|---|---|
| A (gate, ratio 0.9) | 11,432 | 7,454,931 | 1,478,798 | 8,584 | 1,432,989 | 59 s |
| B (no gate, ratio 0.8) | 11,435 | 2,301,937 | — | 9,475 | 1,213,788 | 130 s |
| LoMa itself (for scale) | 10,041 | 7,144,634 | — | 10,032 | 7,032,858 | 36 min |

Errors are in source-image pixels (`reprojStats` works in original-image
coordinates), so the runs compare directly.

## Against SIFT alone

The SIFT `--progressive` baseline placed 645 of 678. Its feature pass 1 brought
in one image; passes 2 and 3 were undone. The 33 images it left out are all
A7iii; all 37 equirectangular images went in. Hybrid A against it:

- +31 images (676 against 645): LoMa places them and SIFT keeps 673 of them
  with points.
- +40% points (382k against 273k) and +38% observations.
- Error 1.62 against 1.45 px. Slightly worse, on 31 harder images that SIFT
  alone could not place at all.

So the hybrid sits between the two frontends as the plan intended: LoMa's
coverage, close to SIFT's precision. It has more points than SIFT alone, but
not LoMa's 607k.

## Hybrid A, the stage itself

`hybrid.txt`: SIFT on 676 images at 2400 px, 11,432 pairs, 8,584 verified.
The epipolar gate kept 1,478,798 of 7,454,931 putative matches (19.8%), and
verification kept 1,432,989 of those (96.9%). SIFT dropped 3 of LoMa's 676
images; all 3 were put back pose-only. The whole stage took 59 s.

- Precision: the mean error fell from 3.01 to 1.62 px, 1.9x tighter.
- Density went the wrong way. Points fell 37% (607k to 382k) and
  observations 44%, although SIFT keeps 8192 features per image against
  LoMa's 4096. The plan expected SIFT to add density.
- Placement: same as LoMa (676/678), as expected for stage 3 of the plan.

## What B says about the gate

- The gate is not what costs the points. Without it there are 22% fewer
  points (298k against 382k) and 21% fewer observations. With the ratio
  relaxed to 0.9, the gate passes 1.48M matches, 97% of which verify. At 0.8
  with no gate, 2.30M putatives verify at 53%.
- B's error is a little lower (1.51 against 1.62 px). It keeps fewer and
  stronger tracks: the same trade a stricter ratio test always makes.
- Verification was faster under the gate: 59 s against 130 s. RANSAC has far
  fewer outliers to sort through.

## Why the points went down

SIFT verifies about 1.4M inliers on this capture against LoMa's 7.0M:
five times fewer per pair, on texture-poor interior walls. The SIFT model
has fewer points than LoMa's for that reason, whatever the gate does. Levers
not yet tried, in order of cost:

1. Relax the ratio further under the gate (0.95, `SS_SFM_HYBRID_RATIO`). The
   gate already does the rejecting.
2. A lower SIFT contrast floor (`--peak-threshold` ÷2, as progressive F3
   does) or more features. Flat walls are where SIFT finds nothing.
3. Keep LoMa's points as well, so SIFT adds precision without taking density
   away. That goes against the plan's "only poses cross over".

## Notes on the runs

- A first matched LoMa again from scratch (36 min). The seeded `matches.bin`
  was refused because the copy changed the feature digest. B used
  `--reuse-matches keep` over A's matches; the log says so.
- The LoMa ladder placed 676 on its first attempt (20 px). Nothing came after,
  so it skipped from 11.9 px to 3 px. Feature passes 1 and 2 on the 2
  unplaced images were undone.

## Where the queue stands (paused by request, 2026-10-10 17:41 EDT)

- Done: loma_prog, hybrid (A), hybrid_noguide (B), all above.
- Stopped part-way: sift_prog (the SIFT `--progressive` baseline, base build).
  Extraction and matching were finished; the ladder had placed 642 of 678 by
  attempt 3 of 12 (14.2 px, 13,321 verified pairs). It resumes in place.
- Not started: hybrid_extreme (A at `--quality extreme`), then A with
  `SS_SFM_HYBRID_RATIO=0.95` and `1.0` over A's matches.
- `resume_queue.sh` runs the rest in that order; `queue.txt` is the run log.

## Build 2 (42e547c3): density knobs

All five use A's flags and A's LoMa matches (`--reuse-matches keep`), and
build 2's binary. The environment for each is in its `cmd.txt`.

| run | env | SIFT kept / restored | points | observations | mean px | median px |
|---|---|---|---|---|---|---|
| hybrid (A, build 1) | — | 673 / 3 | 382,112 | 1,014,341 | 1.616 | 1.304 |
| hybrid_r095 | ratio 0.95 | 674 / 2 | 423,323 | 1,111,855 | 1.664 | 1.350 |
| hybrid_peak | ratio 0.95, peak ×0.5 | 674 / 2 | 435,805 | 1,127,880 | 1.726 | 1.425 |
| hybrid_nocc | ratio 0.95, no cross-check | 676 / 0 | 494,144 | 1,264,779 | 1.739 | 1.424 |
| hybrid_learned | ratio 0.95, + LoMa tracks | 675 / 1 | 1,051,321 | 2,803,362 | 2.104 | 1.784 |

| run | putative | after gate | verified pairs | inliers |
|---|---|---|---|---|
| hybrid_r095 | 13,417,538 | 1,665,829 | 9,156 / 11,442 | 1,603,760 |
| hybrid_peak | 14,206,618 | 1,684,790 | 9,264 / 11,436 | 1,621,794 |
| hybrid_nocc | 34,912,322 | 2,060,602 | 10,233 / 11,444 | 1,949,571 |
| hybrid_learned | 13,423,959 | 1,666,215 | 9,162 / 11,446 | 1,604,142 |

Every hybrid stage took 64–69 s.

- **Cross-check off is the best SIFT-only lever here.** It gives +29% points
  over A (494k), and SIFT keeps all 676 images, so none need restoring. The
  error rises 0.12 px. The gate does the work the cross-check did: 2.6x more
  putatives, still 95% verified after the gate.
- **Ratio 0.95** gives +11% points for +0.05 px.
- **Peak ×0.5** gives +3% on top of 0.95 for +0.06 px. At an 8,192-feature cap
  a lower floor mostly swaps which features are kept. Raising
  `SS_SFM_HYBRID_FEATURES` alongside it is the untested next step.
- **LoMa tracks on the SIFT poses** give 1.05M points, 1.7x LoMa alone, and
  the mean drops from LoMa's 3.02 to 2.10 px. The run exits 3 (PARTIAL)
  because its mean is over 2 px; it is not a failure. The 628k LoMa points are
  triangulated with no BA of their own, so they carry LoMa's keypoint
  precision. For a splat seed this is the densest model of all the runs.

In every run, the baseline included, LoMa's third feature pass ran out of GPU
memory (10 GB asked of an 8 GB card) and was undone. The metric frame was not
applied either: only the 37 equirect images carry GPS, and they do not spread
out. Neither comes from the hybrid.

## Extreme quality (hybrid_extreme, build 2, defaults)

A fresh workspace with `--quality extreme`: LoMa at 2400 px and 8,192
features, SIFT at 3200 px and 16,384, with the default ratio 0.9 and
cross-check on.

| | images | points | observations | mean px | median px |
|---|---|---|---|---|---|
| LoMa at extreme, before the hybrid stage | 676 | 1,096,876 | — | 2.09 | — |
| hybrid_extreme | 676 (669 SIFT, 7 restored) | 558,674 | 1,443,999 | 1.234 | 1.005 |

- The lowest error of every run, below SIFT alone (1.45 px), with twice its
  points and 31 more images.
- Time: 2:37:05 in all. Extraction took 10 min, LoMa matching of 9,607 pairs
  2:08:47, and mapping 18 min. The hybrid stage was 107 s of that: 11,567
  pairs, 9,187 verified, 1,956,766 inliers, and the gate kept 2.01M of 12.09M.
- LoMa's feature pass 1 ran out of GPU memory (7.97 GiB asked) and was undone.
  LoMa had placed 676 images at the first attempt anyway.

## Summary

| run | images | points | mean px |
|---|---|---|---|
| LoMa alone, high | 676 | 606,874 | 3.015 |
| SIFT alone, high | 645 | 273,451 | 1.454 |
| hybrid A, high | 676 | 382,112 | 1.616 |
| hybrid, high, ratio 0.95, no cross-check | 676 | 494,144 | 1.739 |
| hybrid, high, ratio 0.95, + LoMa tracks | 676 | 1,051,321 | 2.104 |
| hybrid, extreme, defaults | 676 | 558,674 | 1.234 |

On this capture the stage does what the plan set out. It keeps every camera
LoMa placed, 31 more than SIFT alone, and halves LoMa's error. For points the
choice is a trade:

- Use the SIFT points for precision. At `extreme` they beat SIFT alone on
  every count.
- Add LoMa's tracks for the densest seed (1.05M points at high).

On density, cross-check off is the best single knob (+29%), and the epipolar
gate makes it safe.

Still open: step 2 of the plan (guided matching for the pairs SIFT still
loses), `SS_SFM_HYBRID_FEATURES` above 8,192 at high, and cross-check off at
extreme.

## Build 3 (2a588d37): the new defaults

`hybrid_b3` has A's flags over A's LoMa matches, with no `SS_SFM_HYBRID_*`
set. The build passes the same five sfm tests.

| run | SIFT kept / restored | points | observations | mean px | median px | inliers |
|---|---|---|---|---|---|---|
| hybrid_nocc (build 2, env) | 676 / 0 | 494,144 | 1,264,779 | 1.739 | 1.424 | 1,949,571 |
| hybrid_b3 (build 3, defaults) | 676 / 0 | 494,055 | 1,264,384 | 1.745 | 1.429 | 1,949,508 |

It reproduces hybrid_nocc to within 0.02% of points. The leftover difference
is the LoMa ladder's own run-to-run noise: its input model held 606,561
points against 606,875.

## Build 4 (81859d4b): band matching

Build 4 passes the five sfm tests and `preset_roundtrip_test` (the GUI switch).
Every run has hybrid_b3's command over A's LoMa matches; only the binary, the
workspace and the env change. All four placed 676 of 678 images.

| run | env | SIFT kept / restored | points | observations | mean px | median px | inliers | stage s |
|---|---|---|---|---|---|---|---|---|
| hybrid_b3 (build 3) | — | 676 / 0 | 494,055 | 1,264,384 | 1.745 | 1.429 | 1,949,508 | 88 |
| hybrid_b4_gate | — | 676 / 0 | 494,221 | 1,264,760 | 1.744 | 1.428 | 1,949,622 | 68 |
| hybrid_band | MATCH=band | 676 / 0 | 1,671,280 | 3,496,364 | 2.126 | 1.879 | 10,685,406 | 202 |
| hybrid_band_r08 | band, RATIO=0.8 | 676 / 0 | 1,115,680 | 2,485,546 | 2.062 | 1.791 | 4,935,035 | 130 |
| hybrid_band_r095 | band, RATIO=0.95 | 676 / 0 | 1,911,700 | 3,916,761 | 2.124 | 1.879 | 16,185,318 | 349 |

| run | putative | kept by band / gate | verified pairs |
|---|---|---|---|
| hybrid_b4_gate | 34,921,988 | 2,060,901 | 10,246 / 11,449 |
| hybrid_band | 20,909,415 | 12,021,340 | 11,356 / 11,428 |
| hybrid_band_r08 | 8,495,325 | 5,423,538 | 11,360 / 11,437 |
| hybrid_band_r095 | 36,081,163 | 18,340,867 | 11,381 / 11,448 |

- **b4 with no env reproduces b3** (494,221 against 494,055 points, 1.744
  against 1.745 px).
- **Band matching multiplies SIFT's own density**: 3.4x points at the default
  ratio (1.67M) and 3.9x at 0.95 (1.91M). That is more than LoMa's tracks
  added in hybrid_learned (1.05M), and these are all SIFT points with
  SIFT-refined poses. Nearly every pair verifies (99%+).
- **The price is error**: 2.06–2.13 px mean, against 1.74 at the gate's
  default. Every band run is over the 2 px line the run report uses, so all
  three exit 3 (PARTIAL). The model is complete; no run failed. Band
  matching takes candidates the ratio test alone would drop, and a share of
  them sit at the edge of the 3 px gate.
- **Ratio**: 0.8 cuts points 33% for only 0.06 px. 0.95 adds 14% points at
  the same error as 0.9, for 1.7x the stage time (349 s). Of the band runs,
  0.95 gives the most points for its error.
- Stage time stays under 6 min at every ratio, well inside the 10 min limit.

## Build 5 (581b8fcd): band matching with mutual neighbours

Build 5 passes the five sfm tests and `preset_roundtrip_test`. Same command
and seeding as build 4 (A's LoMa matches, `--reuse-matches keep`). All four
runs placed 676 of 678 images.

| run | env | SIFT kept / restored | points | observations | mean px | median px | inliers | `[hybrid] … kept X of Y` | stage s |
|---|---|---|---|---|---|---|---|---|---|
| hybrid_b5_band | MATCH=band | 676 / 0 | 1,244,335 | 2,718,251 | 2.049 | 1.782 | 5,584,415 | band 6,091,328 of 20,926,750 | 151 |
| hybrid_b5_band_px2 | band, EPIPOLAR_PX=2 | 676 / 0 | 1,454,903 | 3,089,931 | 1.333 | 1.175 | 6,808,491 | band 6,877,032 of 23,436,832 | 163 |
| hybrid_b5_band_r08_px2 | band, RATIO=0.8, PX=2 | 676 / 0 | 891,486 | 1,999,329 | 1.323 | 1.168 | 3,094,842 | band 3,111,580 of 10,633,255 | 78 |
| hybrid_b5_gate_px2 | EPIPOLAR_PX=2 | 674 / 2 | 422,666 | 1,107,172 | 1.364 | 1.159 | 1,547,132 | gate 1,582,450 of 34,922,098 | 50 |

Exit codes: hybrid_b5_band exits 3 (mean over 2 px). The three 2 px runs exit 0.

- **A 2 px band is the best setting so far.** It gives 1.45M points at
  1.33 px: more points than any build 4 band run except 0.95, at the lowest
  error of any high-quality run. It is below SIFT alone (1.45 px, 273k) with
  5.3x the points, and close to the extreme run's 1.23 px. Of its matches,
  99% verify.
- **Mutual neighbours at 4.5 px** cut points 26% against build 4's band
  (1.24M against 1.67M) for only 0.08 px. The band width matters more than
  the mutual check: at 4.5 px, borderline matches are what raise the error.
- **Ratio at 2 px**: 0.8 gives the same error as 0.9 (1.32 against 1.33 px)
  with 39% fewer points. Keep 0.9.
- **The control** (gate at 2 px, no band) shows that 2 px alone tightens the
  error (1.36 px against 1.74 at 4.5 px) but costs points (423k against
  494k). It also lets SIFT lose 2 images. The band is what brings the density.
- Recommended default from this capture: band matching, mutual on, ratio
  0.9, band at 2 px.

## Build 6 (53351e94): band matching by default

Build 6 passes the five sfm tests and `preset_roundtrip_test`. The first
compile stopped with MSVC out of heap space (C1002/C1060) on four large
translation units at once. Free RAM was 32 GB afterwards, and the unchanged
retry built clean. No `SS_SFM_HYBRID_*` is set in either run.

| run | SIFT kept / restored | points | observations | mean px | median px | inliers | `[hybrid] … kept X of Y` | stage s |
|---|---|---|---|---|---|---|---|---|
| hybrid_b5_band_px2 (build 5, env) | 676 / 0 | 1,454,903 | 3,089,931 | 1.333 | 1.175 | 6,808,491 | band 6,877,032 of 23,436,832 | 163 |
| hybrid_b6 (high, defaults) | 676 / 0 | 1,455,018 | 3,090,098 | 1.332 | 1.174 | 6,807,779 | band 6,876,244 of 23,425,283 | 139 |
| hybrid_extreme (build 2, gate) | 669 / 7 | 558,674 | 1,443,999 | 1.234 | 1.005 | 1,956,766 | gate 2,014,239 of 12,090,716 | 107 |
| hybrid_b6_extreme (extreme, defaults) | 676 / 0 | 2,514,042 | 5,251,633 | 1.003 | 0.887 | 11,346,871 | band 11,471,252 of 43,459,196 | 179 |

hybrid_b6_extreme reused hybrid_extreme's LoMa matches (9,603 pairs), so its
total of 23 min has no matching time. Both exit 0, and all runs placed 676/678.

- **hybrid_b6 reproduces b5_band_px2** with no env: 1,455,018 against
  1,454,903 points, 1.332 against 1.333 px.
- **At extreme, the new default gives 4.5x the points of the old extreme run
  at a lower error**: 2.51M points at 1.003 px (0.887 median), against 559k at
  1.234 px. SIFT kept all 676 images (the old run restored 7). This is the
  best model of every run: 4.1x LoMa alone's points (607k, 3.02 px) at a
  third of its error, and 9.2x SIFT alone's (273k, 1.45 px) at lower error.
- The hybrid stage stays cheap at extreme: 179 s for 11,581 pairs and 43M
  putative matches.
- LoMa's feature pass 1 ran out of GPU memory again, as in every extreme run.

## Build 7 (00ed48f9): the lost structure

Build 7 passes the five sfm tests and `preset_roundtrip_test`.
`results/tools/hybrid_diag.py` fits each model's camera centres onto the LoMa
model's (Sim3, by image name). It then counts the points that land within 1%
of the scene extent of some LoMa point. Full output is in
`diag_b7/existing.txt` and `diag_b7/runs.txt`. The GUI model is the user's,
read only, from `H:\Hershy Site GS\GS runs\a7iii_inside\sparse\0`.

| model | points | 2-view share | near, all | near, 2-view | near, 3-view | near, 4+ |
|---|---|---|---|---|---|---|
| loma_prog (reference) | 606,874 | 63.0% | 100.0% | 100.0% | 100.0% | 100.0% |
| hybrid_b4_gate | 494,221 | 73.2% | 97.0% | 96.0% | 99.7% | 100.0% |
| hybrid_b6 (band) | 1,455,018 | 91.2% | 80.8% | 79.3% | 95.1% | 99.7–100% |
| sift_prog | 273,451 | 69.0% | 99.0% | 98.8% | 99.2% | 99.6–99.8% |
| GUI model | 1,451,313 | 91.3% | 80.4% | 78.8% | 95.2% | 99.7–100% |
| hybrid_b7_band_mv3 | 127,514 | — | 97.1% | — | 95.1% | 99.7–100% |
| hybrid_b7_gate_mv3 | 132,399 | — | 99.9% | — | 99.8% | 100.0% |

| run | env | points | observations | mean px | median px | dropped (< 3 views) | `[hybrid] … kept X of Y` | stage s |
|---|---|---|---|---|---|---|---|---|
| hybrid_b7_band_mv3 | MIN_VIEWS=3 | 127,514 | 499,536 | 1.778 | 1.577 | 1,326,983 | band 6,872,255 of 23,420,370 | 195 |
| hybrid_b7_gate_mv3 | MATCH=gate, MIN_VIEWS=3 | 132,399 | 555,958 | 1.817 | 1.563 | 361,596 | gate 2,059,981 of 34,893,981 | 64 |

Both placed 676/678; SIFT kept all 676.

- **The theory holds.** The band models' extra points are almost all 2-view
  tracks (91%), and 21% of those sit off the structure: about 275k misplaced
  points in b6 and the GUI model alike. The GUI model is b6's profile exactly.
  The cameras are not the problem: b6 agrees with LoMa to a median
  0.0000 of extent and 0.007°.
- **Band matching adds almost no multi-view structure.** b6 has 127k points
  in 3+ views, against b4_gate's 132k. With 2-view tracks removed, band and
  gate are about the same size (128k against 132k), and the gate's are
  cleaner (99.9% against 97.1%). Even band's 3-view tracks are only 95% near.
- **Dropping 2-view tracks is too blunt.** It leaves fewer points than SIFT
  alone (273k at 99.0%), and the mean error rises to about 1.8 px, because
  longer tracks carry the larger residuals. b4_gate's 2-view tracks are 96%
  near. Those are worth keeping; band's 79% are not.
- So the cleanest dense default here is still gate matching with every track
  (b4_gate: 494k points, 97% near, 1.74 px). Band density would need a
  per-track check against the other views, not a blanket view count.
