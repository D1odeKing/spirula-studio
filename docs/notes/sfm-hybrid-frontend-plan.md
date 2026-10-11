# Hybrid frontend — plan

Not started. An experimental, opt-in stage after progressive alignment: a
learned frontend (LoMa) decides where the cameras are, then SIFT is matched
under that geometry and gives the model its precision and point density.

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
