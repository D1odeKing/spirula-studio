# Hybrid frontend — plan

Status: steps 0 to 2 are in as `spirula sfm auto --hybrid-sift` (`sfm/Hybrid.cpp`),
with step 2 as guided matching: SIFT is searched along the epipolar lines the
learned model's poses predict.
An experimental, opt-in stage after mapping: a learned frontend (LoMa) decides
where the cameras are, then SIFT is matched under that geometry and gives the
model its precision.

## What runs

After the learned run's mapping and finishing passes: SIFT at the quality's own
size on every registered image (`features.sift/`); pairs the learned frontend
verified, plus each image's 20 best partners by shared points; band matching:
each keypoint is compared only with the other image's keypoints within 2/3 x
`--max-error` of the arc its ray sweeps there, epipole to vanishing point
(angles on bearings, so an equirectangular image is searched like a pinhole
one), the ratio test (0.9) over those alone from both sides, keeping mutual
nearest neighbours; verification; triangulation against the learned poses and
a free bundle adjustment (`Mapper::retriangulate`), then the finishing passes
again. Any image SIFT cannot hold is put back at its learned pose, carried into
the SIFT frame by a Sim3, with no points. A failure keeps the learned model.
`SS_SFM_HYBRID_MATCH=gate` is the earlier form: the global nearest neighbour
(ratio 0.95, no cross-check), kept within 1.5 x `--max-error` of its epipolar
plane. The GUI's "Refine with SIFT" (Advanced, learned features only) passes
`--hybrid-sift`.
`hybrid.txt` records the stage. Experiment knobs, all `SS_SFM_HYBRID_*`:
`MATCH` (`band`, `gate`, `none`), `GUIDED`, `RATIO`, `CROSS_CHECK`,
`BAND_MUTUAL`, `PEAK_SCALE`, `FEATURES`, `IMAGE_SIZE`, `EPIPOLAR_PX`, and `LEARNED_POINTS=1`, which also triangulates the learned
tracks against the final poses (no BA) for a denser seed.

## Measured: a7iii_inside

678 images (37 equirectangular 11904x5952, 641 A7iii pinhole), RTX 4060 8 GB.
Errors in source pixels.

| run | images | points | mean px |
|---|---|---|---|
| LoMa `--progressive`, high | 676 | 606,874 | 3.015 |
| SIFT `--progressive`, high | 645 | 273,451 | 1.454 |
| hybrid, high, ratio 0.9 + cross-check | 676 | 382,112 | 1.616 |
| hybrid, high, gate, ratio 0.95, no cross-check | 676 | 494,144 | 1.739 |
| hybrid, high, + `LEARNED_POINTS` | 676 | 1,051,321 | 2.104 |
| hybrid, extreme, ratio 0.9 + cross-check | 676 | 558,674 | 1.234 |
| hybrid, high, band 0.8, one per keypoint | 676 | 1,115,680 | 2.062 |
| hybrid, high, band 0.9, one per keypoint | 676 | 1,671,280 | 2.126 |
| hybrid, high, band 0.95, one per keypoint | 676 | 1,911,700 | 2.124 |
| hybrid, high, band 0.9, mutual, 4.5 px | 676 | 1,244,335 | 2.049 |
| hybrid, high, band 0.9, mutual, 2 px (default) | 676 | 1,454,903 | 1.333 |
| hybrid, high, band 0.8, mutual, 2 px | 676 | 891,486 | 1.323 |
| hybrid, high, gate, 2 px | 674 + 2 restored | 422,666 | 1.364 |
| hybrid, extreme, band (default) | 676 | 2,514,042 | 1.003 |

The stage itself is about a minute at high. LoMa leaves out 2 images where SIFT
leaves out 33, so step 0's question is answered: the learned frontend places
images SIFT cannot, and SIFT keeps all but a few of them with points. Without
the gate (ratio 0.8, cross-check) there are 22% fewer points and verification
takes twice as long.

The band's width is what matters. At 4.5 px (1.5 x `--max-error`) a ratio test
over a few dozen candidates passes near neighbours along the line, and the
median error rises from 1.43 to 1.78 px however the ratio is set. At 2 px it
gives 2.9 times the gate's points at 1.33 px against 1.74, more points than
LoMa's own model at well under half its error, in about 2.5 minutes. Ratio 0.8
loses 39% of the points for no gain; a 2 px gate loses 15% and two images.

At extreme the band gives 4.5 times the old extreme run's points at 1.00 px
against 1.23, keeps all 676 images (the old run restored 7), in 3 minutes.

Open: more SIFT features at high.

## Why

The two frontends fail differently. LoMa matches pairs SIFT cannot (wide
baselines, weak or repeated texture) and so tends to place more cameras; SIFT
keypoints are sub-pixel and several times as many, which is what a low
reprojection error and a dense splat initialisation need. A run today picks
one (`sfm-progressive-alignment.md`, "One frontend and one matcher per run");
this stage is the deliberate exception, and only behind its own switch.

## What LoMa's result can do for SIFT, weakest to strongest

1. **Pair choice.** SIFT matches only the pairs LoMa verified, or ranks them
   first in pair selection. Faster, fewer false pairs, but it cannot make SIFT
   match a pair it fails on. Not worth a stage by itself.
2. **Guided matching.** Each LoMa-verified pair carries its two-view geometry;
   a SIFT candidate is accepted only near its epipolar line, which lets the
   ratio test relax where texture repeats. COLMAP's `guided_matching` is the
   same idea from the pair's own geometry. Nothing in `src/sfm/` does this
   yet: it is new matcher work (a per-pair F or E handed to the matcher).
3. **Fixed poses.** Keep LoMa's cameras, triangulate SIFT tracks against them,
   then bundle-adjust with the SIFT observations. Every camera LoMa placed is
   kept; only poses cross over, never LoMa's feature rows, so the two feature
   sets never need to agree. Upstream PR #144 ("new points for a solve's own
   cameras", `sfm --poses`) overlaps; read it before writing a second one.

The stage worth building is 3, then 2 on top of it.

## Order

0. **Measure first.** A LoMa progressive run on a capture where ALIKED or SIFT
   leaves images out, against that run: images placed, models, mean error,
   points. If LoMa leaves the same images out, this stage cannot bring them in.
1. Fixed-pose SIFT refinement (3), as `--hybrid-sift` or a progressive stage 3.
2. Guided matching (2) for the pairs SIFT still loses.

## Costs and risks

- Two extractions and two matching passes; LoMa has run out of memory at
  3869 px on an 8 GB card (`Progressive.cpp`'s learned size cap).
- Mixed precision: LoMa's keypoints are coarser, so the poses it hands over
  are only a starting point; the SIFT bundle adjustment must be free to move
  them, or the refinement inherits LoMa's error.
- `DatasetPlan` needs the stage's settings in `model_fields`, and resume a
  signature of its own.
