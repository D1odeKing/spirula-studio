# Hybrid frontend — plan

Status: steps 0 to 2 are in as `spirula sfm auto --hybrid-sift` (`sfm/Hybrid.cpp`),
with step 2 as an epipolar gate: SIFT's putative matches are kept only near the
epipolar lines the learned model's poses predict.
An experimental, opt-in stage after mapping: a learned frontend (LoMa) decides
where the cameras are, then SIFT is matched under that geometry and gives the
model its precision.

## What runs

After the learned run's mapping and finishing passes: SIFT at the quality's own
size on every registered image (`features.sift/`); pairs the learned frontend
verified, plus each image's 20 best partners by shared points; the global
nearest neighbour (ratio 0.95, no cross-check), kept within 1.5 x
`--max-error` of its epipolar plane (angles on bearings, so an
equirectangular image is gated like a pinhole one); verification; triangulation against the learned poses and
a free bundle adjustment (`Mapper::retriangulate`), then the finishing passes
again. Any image SIFT cannot hold is put back at its learned pose, carried into
the SIFT frame by a Sim3, with no points; when fewer than two shared poses tie
the frames, the log says how many images could not be put back. A failure
keeps the learned model, with the reason translated like the rest of the log.
After the stage the image list counts SIFT's keypoints, and the dataset
screen's feature reel shows `features.sift/` while it is written.
`SS_SFM_HYBRID_MATCH=band` searches the band instead (below: it misplaces
points); band matching and verification then split the cores. The GUI's "Refine with SIFT" (Advanced, learned features only) passes
`--hybrid-sift`.
`hybrid.txt` records the stage. Experiment knobs, all `SS_SFM_HYBRID_*`:
`MATCH` (`gate`, `band`, `none`), `GUIDED`, `RATIO`, `CROSS_CHECK`,
`BAND_MUTUAL`, `MIN_VIEWS`, `PEAK_SCALE`, `FEATURES`, `IMAGE_SIZE`,
`EPIPOLAR_PX`, and `LEARNED_POINTS=1`, which also triangulates the learned
tracks against the final poses (no BA) for a denser seed.

## Measured: a 678-image interior

678 images (37 equirectangular 11904x5952, 641 pinhole), RTX 4060 8 GB.
Errors in source pixels.

| run | images | points | mean px |
|---|---|---|---|
| LoMa `--progressive`, high | 676 | 606,874 | 3.015 |
| SIFT `--progressive`, high | 645 | 273,451 | 1.454 |
| hybrid, high, ratio 0.9 + cross-check | 676 | 382,112 | 1.616 |
| hybrid, high, gate, ratio 0.95, no cross-check (default) | 676 | 494,144 | 1.739 |
| hybrid, high, + `LEARNED_POINTS` | 676 | 1,051,321 | 2.104 |
| hybrid, extreme, ratio 0.9 + cross-check | 676 | 558,674 | 1.234 |
| hybrid, high, band 0.8, one per keypoint | 676 | 1,115,680 | 2.062 |
| hybrid, high, band 0.9, one per keypoint | 676 | 1,671,280 | 2.126 |
| hybrid, high, band 0.95, one per keypoint | 676 | 1,911,700 | 2.124 |
| hybrid, high, band 0.9, mutual, 4.5 px | 676 | 1,244,335 | 2.049 |
| hybrid, high, band 0.9, mutual, 2 px | 676 | 1,454,903 | 1.333 |
| hybrid, high, band 0.8, mutual, 2 px | 676 | 891,486 | 1.323 |
| hybrid, high, gate, 2 px | 674 + 2 restored | 422,666 | 1.364 |
| hybrid, extreme, band | 676 | 2,514,042 | 1.003 |

The stage itself is about a minute at high. LoMa leaves out 2 images where SIFT
leaves out 33, so step 0's question is answered: the learned frontend places
images SIFT cannot, and SIFT keeps all but a few of them with points. Without
the gate (ratio 0.8, cross-check) there are 22% fewer points and verification
takes twice as long.

Mean reprojection error does not show whether a point is in the right place.
A wrong match that slides along its epipolar line triangulates at the wrong
depth with no error in two views, and band matching makes exactly those: its
1.45M points at 1.33 px are 91% two-view tracks, and the GUI showed no room.
`results/tools/hybrid_diag.py` aligns a model to the LoMa one by camera centres
and counts its points within 1% of the scene extent of a LoMa point:

| model | points | two-view | near structure | two-view near |
|---|---|---|---|---|
| LoMa | 606,874 | 63% | 100% | 100% |
| SIFT alone | 273,451 | 69% | 99.0% | 98.8% |
| hybrid, gate (default) | 494,221 | 73% | 97.0% | 96.0% |
| hybrid, band 2 px | 1,455,018 | 91% | 80.8% | 79.3% |

The cameras were never at fault: the band model's centres agree with LoMa's to
under 0.0001 of the extent and 0.007 deg. Its tracks of three or more views
number about the gate's (128k against 132k), so the band adds almost nothing
but two-view points. Dropping two-view points (`MIN_VIEWS=3`) cleans either
model but leaves fewer points than SIFT alone, at about 1.8 px. So the gate is
the default; the band would need a per-point check against the learned
model's depth before it could be.

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
   same idea from the pair's own geometry; `bandMatch` in `sfm/Hybrid.cpp`
   is this, on bearings from the learned poses.
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
