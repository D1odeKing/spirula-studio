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
| hybrid (A) | 676/678 | 1 | 382,112 | 1,014,341 | 1.616 | 1.304 | 0:59:43 |
| hybrid_noguide (B) | 676/678 | 1 | 297,599 | 803,578 | 1.511 | 1.204 | 0:23:47* |

\* B reused A's LoMa matches, so its total has no matching time.

## Matches per stage

| | pairs | putative | after gate | verified pairs | inliers | stage time |
|---|---|---|---|---|---|---|
| A (gate, ratio 0.9) | 11,432 | 7,454,931 | 1,478,798 | 8,584 | 1,432,989 | 59 s |
| B (no gate, ratio 0.8) | 11,435 | 2,301,937 | — | 9,475 | 1,213,788 | 130 s |
| LoMa itself (for scale) | 10,041 | 7,144,634 | — | 10,032 | 7,032,858 | 36 min |

Errors are in each run's own extraction pixels: LoMa at 1600 px, SIFT at
2400 px. At LoMa's scale the hybrid's 1.62 px is about 1.08 px.

## Hybrid A, the stage itself

`hybrid.txt`: SIFT on 676 images at 2400 px, 11,432 pairs, 8,584 verified.
The epipolar gate kept 1,478,798 of 7,454,931 putative matches (19.8%), and
verification kept 1,432,989 of those (96.9%). SIFT dropped 3 of LoMa's 676
images; all 3 were put back pose-only. The whole stage took 59 s.

- Precision: the mean error fell from 3.01 to 1.62 px, about 2.8x tighter
  once the pixel scales are matched.
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
