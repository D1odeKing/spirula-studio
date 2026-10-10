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

## Notes on the runs

- A first matched LoMa again from scratch (36 min). The seeded `matches.bin`
  was refused because the copy changed the feature digest. B used
  `--reuse-matches keep` over A's matches; the log says so.
- The LoMa ladder placed 676 on its first attempt (20 px). Nothing came after,
  so it skipped from 11.9 px to 3 px. Feature passes 1 and 2 on the 2
  unplaced images were undone.
