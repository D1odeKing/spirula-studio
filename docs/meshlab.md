# Mesh-lab: measurement-grade meshing

The splat mesher (`spirula mesh`, `src/mesh/MeshingHost.cpp`) looks good but is
lumpy on low-texture subjects and has no metric scale. Mesh-lab is what it takes
to **measure on** a mesh:

1. a metric scale you can trust — `spirula scale`,
2. a mesher that works from the triangulated RoMa dense cloud — `spirula mesh-dense`,
3. a refinement that uses monocular normals (Marigold v2 / MoGe) for flat faces
   and edges without letting them move the surface uncontrolled — `spirula mesh-refine`,
4. a benchmark that checks every change — `spirula mesh-eval` and
   `tools/meshlab/bench.ps1`.

## The branch

Mesh-lab lives on the long-lived branch `mesh-lab` of the fork. It is kept
current with upstream by **merging** `upstream/master` (never rebasing) and is
not meant to merge to master.

```
git remote add upstream https://github.com/harry7557558/spirula-studio.git
git fetch upstream
git merge upstream/master
build_develop.bat -DSS_BACKEND=vulkan          # RoMa dense needs the Vulkan nn layer
build_vulkan\maxflow_test && build_vulkan\graphcut_mesh_test
build_vulkan\normal_refine_test && build_vulkan\rescale_test
pwsh tools/meshlab/bench.ps1 -Dataset DTU -DataRoot <data>   # then compare results.csv
```

New code goes in new files (`src/mesh/*.cpp` and `src/*/tests/*.cpp` are
globbed). Everything is portable host C++, so it builds under both backends.

### Upstream touch points

The conflict checklist for every merge. Nothing else upstream is edited.

| File | Change |
|---|---|
| `src/mesh/MeshingHost.cpp` | the anonymous namespace closes before `merge_vertices` instead of after `improve_mesh_quality`; the eight cleanup passes lose `static`; `#include "mesh/MeshClean.h"` |
| `src/app/Main.cpp` | `#include "app/cli/MeshLabCli.h"` and `SS_MESHLAB_TOOL_ROWS` in `tools()`, both under `SS_TOOL_MESHLAB` |
| `cmake/SsApps.cmake` | the four `app/cli/*_main.cpp` sources and `SS_TOOL_MESHLAB=1`, after the mesh block |
| `src/app/gui/MeshJob.h`, `MeshPreset.cpp`, `MeshRunner.cpp`, `GuiApp.h`, `GuiApp.cpp` | the GUI integration and the ruler, listed under "GUI" below |
| `src/app/gui/MeshJob.cpp` | `mesh_job_outputs` returns `meshlab_dense_outputs` (`MeshLabOutputs.h`) for a dense job |
| `src/app/gui/tests/preset_roundtrip_test.cpp` | the dense output names |

Two deliberate departures from the plan, both to keep that list short: the
tool names and entry points live in `src/app/cli/MeshLabCli.h` rather than
`src/app/Tools.h`, and the strings are a catalog of their own,
`src/i18n/catalog/MeshLab.h`, rather than rows in `Mesh.h`. Both are complete
in all 13 languages.

## Frames

Every mesh-lab tool reads and writes the dataset's **file frame**: the frame
`dense/roma.ply` is in, which for a transforms.json is the JSON's own frame
(`applied_transform` applied), and for COLMAP is COLMAP's world.

`src/mesh/MeshViews.h` turns `parse_dataset()`'s cameras into that frame. The
parser hands out c2w in the raw frame (`raw_to_file` undone, centre `none`), so
with `file = A raw + t` and `A = s Q`:

- camera centre `C_file = A C_raw + t`,
- rotation `R_file = R_raw Q^T`, where `R_raw` is the CV world-to-camera
  rotation (`app/DenseProcessing.cpp`'s GL-to-CV conversion),
- one file-frame unit is `1/s` camera-space units, which only depths see.

A **splat mesh** is in its run's training frame, `p_train = k (p_raw - c)`
(`scene_transform.json`). `--run <dir>` on `mesh-eval` and `mesh-refine` maps
it into the file frame first (`meshing::train_to_file`).

Normal maps are 8-bit PNG/JPG, `n = byte/127.5 - 1`, black where invalid (the
trainer's `gt_normal_valid` rule), CV camera frame. A map at another size than
the camera stands for the same lens scaled. A guide is turned to face its
camera, so the sign convention of the model does not matter.

## `spirula mesh-eval`

```
spirula mesh-eval mesh.ply --ref gt.ply --tau 0.002,0.005 [--icp] [--crop x0 y0 z0 x1 y1 z1]
                  [--transform M.txt] [--run R --dataset D] [--measurements pairs.json]
```

- **Accuracy** (mesh to reference) and **completeness** (reference to mesh):
  mean and median over `--samples` area-uniform points of each surface; a point
  reference is used as is. Chamfer is their mean of means.
- **F1** at each `--tau`: precision and recall count every point in the crop,
  including those past `--max-dist`.
- `--icp` refines the alignment by rigid point-to-plane ICP, trimmed at three
  times the median residual. The final 3x4 is in `metrics.json`.
- `<mesh>_eval/metrics.json` and `<mesh>_eval/<mesh>_error.ply`, vertex
  colours blue (0) to red (`--error-clip`, default the largest tolerance, else
  the 95th percentile).
- `--measurements` takes `{"pairs": [{"name", "p1", "p2", "true_distance"}]}`
  and reports, per pair, the straight distance, the distance between the
  points' closest mesh points, and the error of the latter.
- `--write-mesh` / `--write-samples` write the mesh after `--run` /
  `--transform`, or a point sample of it, which is how `bench.ps1` puts a
  splat mesh into the file frame and feeds ETH3D's evaluator.

This is the inner-loop number. Published numbers come from the official
evaluators below.

## `spirula scale` — metric scale from known distances

```
spirula scale --dataset D --pair "x y z" "x y z" 0.250 [--pair ...] [--pairs file.json] [--apply]
              [--splat <run|ply>] [--mesh m.ply] [--depth-metric] [--out copy_dir]
```

The pairs are in the file frame; the true distance is in metres. Several pairs
are combined by least squares, `s = sum(d m) / sum(m^2)`. Per pair it prints
the measured length, the individual scale and the residual; the **spread** (std
of the individual scales over `s`) is the honest accuracy of the scale, and
`spread / sqrt(n)` is written to `gauge.txt` as `scale_sigma`.

Dry run by default. `--apply` runs `rescale_dataset()` (`src/data/DatasetRescale.h`),
the similarity `p -> s p` about the file-frame origin:

| Artifact | How |
|---|---|
| cameras + sparse points | `sparse_write_filtered` (in place, `.orig` kept), or `sparse_write_copy` with `--out` (COLMAP) |
| `dense/roma.ply` | `write_ply_points(..., moved)` + `dense::commit_edit`, so training still accepts it |
| splat models (`--splat`, opt-in) | `transform_splats` with the scale expressed in the run's training frame, `s p + (1 - s) c + k R u` |
| meshes (`--mesh`) | vertices times `s`, same format |
| `gauge.txt` | `metric 1`, `scale measured`, `scale_sigma`; every other line kept |
| depth PNGs | times `s` only with `--depth-metric`; normalized ones are untouched |
| normal maps | unchanged |

For a transforms.json the sparse writers take the raw frame, so the scale is
written there as `raw' = s raw + (s - 1) A^-1 t`; `rescale_test` checks the
file-frame cloud comes out exactly `s` times.

Training checkpoints other than splat PLYs (optimizer state for `--resume`) are
not rescaled: retrain, or rescale before training.

## `spirula mesh-dense` — Delaunay + visibility graph cut

```
spirula mesh-dense --dataset D [--cloud roma.ply] [-o base] [--max-points 3000000] [--rays 8]
                   [--normals dir] [--depths dir]
```

`src/mesh/GraphCutMesher.cpp`, after Labatut et al. 2009 and Jancosek & Pajdla 2011:

0. **Smooth** (`--smooth 3 --smooth-k 40`): every point moves onto the
   Gaussian-weighted PCA plane of its k nearest neighbours. RoMa's pairs
   triangulate offset layers about one spacing apart, and the cut weaves
   between them: on the van, 9.5% of edges were folds before, 0.9% after.
1. **Thin** the cloud: one point per voxel (`--voxel`), widened until at most
   `--max-points` remain. Local spacing and a PCA normal per point from 10
   nearest neighbours.
2. **Visibility, computed here** (the dense PLY keeps no observations): per
   view every point is splatted into a coarse depth grid (`--grid` cells across
   the longer side) as a disk of its own spacing, and counts as seen when
   within `1 + --depth-eps` of the nearest depth at its centre. A single cell
   per point is not enough: with about one point per cell, the empty front
   cells let the far side through, and a third of a test sphere's rays came
   from cameras behind it. The `--rays` most frontal views are kept per point.
3. **Delaunay** (`compute_delaunay_3d`, adjacency on). Facet `f` of a tet is
   opposite its corner `f`.
4. **Rays**: for each (point, view), walk the tets from the point toward the
   camera. Every crossed facet adds `alpha_vis` to the arc from the
   camera-side tet to the point-side one (it forbids a surface in front of the
   point); the tet holding the camera, or the last before the hull, gets a
   source link. The tet `--sigma` local spacings behind the point gets a sink
   link. Rays run in parallel with atomic float adds.
5. **Hull and shape terms**: each convex-hull facet ties its tet to the source
   (`--hull`), and every facet carries `quality * (perimeter / 3 spacing)^2 * (1 + sliver)`,
   capped, so large and sliver facets are dear to cut.
6. **Min cut**: `src/mesh/MaxFlow.h`, Boykov-Kolmogorov written from the paper
   (maxflow-v3's research-only licence and OpenMVS's AGPL rule both out).
   `src/core/GraphCut.h` is spectral bisection and does not apply.
7. **Extract** the inside/outside facets wound outward. Before that, every
   Delaunay edge whose ring of tets has more than one run of inside tets is
   made manifold by moving the shorter runs outside: two inside tets meeting
   along an edge only is a non-manifold edge no mesh-level pass can split.
   Then faces with an edge past `--long-edge` spacings are dropped and the
   MeshClean passes run (`MeshClean.h`): dedup, split non-manifold vertices,
   floaters, holes (up to 48 edges, which closes what the long-face cut
   opens), degenerate, quality, orient.
8. Vertex colour from the nearest point of the **unthinned** cloud, written by
   `write_mesh` in any format.

Later: persisting the real observations and normals in a dense sidecar
(`Reconstruction.cpp`'s `fuse()`, the `Generation.h` publish list) would be an
upstream touch point, so it waits until the benchmark shows computed visibility
is not enough.

## `spirula mesh-refine` — normal-guided refinement

```
spirula mesh-refine --mesh in.ply --dataset D [--run R] [--anchor dense|none] [--max-shift d]
                    [--plane-snap --plane-tol d] [--normals dir] [--depths dir]
```

`--normals` / `--depths` name the map folders (default `normals/`, `depths/` in the
dataset; the GUI's two fields). Both tools log how many views have each map;
only refine reads normals, and nothing reads depth maps yet.

`src/mesh/NormalRefine.cpp`. Any mesh, splat or dense:

1. **Guide normal per face.** Each view with a normal map rasterizes the mesh
   into a CPU z-buffer (`--raster` px), and every visible face takes the map
   texel under its centroid (the mean of its pixels when it does not own that
   one), turned camera to file frame and toward the camera. Weight: `|cos|` of
   the view angle times `fx / distance`. Per face, the weighted medoid
   direction is found, views more than `--outlier-deg` from it dropped, and the
   rest averaged. Confidence is `agreement^2 * kept weight share`, a quarter of
   that when one view alone saw the face: reflections and diffusion
   hallucinations disagree between views, so they lose their say.
2. **Bilateral normal filter** (Zheng et al. 2011), joint: the range kernel
   reads the GUIDES where both faces have one, so a guide discontinuity is a
   crease that survives (`RefineConfig::sigma_r`). Each pass then pulls a face toward its
   guide by `pull * confidence`.
3. **Vertex update**: per vertex, the 3x3 normal equations of
   `sum_f A_f sum_edges (n_f . (x_j - x_i))^2 + lambda |x_i - a_i|^2`
   (areas normalized), damped Jacobi, `--vertex-iters` per round. `a_i` is the
   input vertex moved along its normal to the nearest dense point within the
   cap (`--anchor dense`), else the input vertex. Every vertex is clamped into
   a ball of `--max-shift` around where it started: **normals shape the
   surface, position stays on triangulated data.**
4. **Plane snap** (off by default): regions grown over confident faces with
   guides within `--plane-deg`, a least-squares plane through their anchors,
   interior vertices projected onto it only when the anchors' RMS is under
   `--plane-tol`.
5. **Output**: the refined mesh (file frame), `<out>_shift.ply` — the
   displacement map: per-vertex signed shift along the normal as a float
   property `shift` plus colour — and `<out>_refine.json` with the mean and max
   shift.

## Benchmarks

Scored with each benchmark's **official protocol**, so the numbers line up with
the published tables (2DGS, GOF, PGSR, GVGS, Gaussian Wrapping, MILo).

| Dataset | Scenes | Ground truth | Metric / evaluator |
|---|---|---|---|
| DTU | 24 37 40 55 63 65 69 83 97 105 106 110 114 118 122 (2DGS-preprocessed COLMAP layout) | structured light, mm | mean Chamfer, the Python port of the official MATLAB script that 2DGS / GOF / PGSR ship, official observation masks |
| Tanks and Temples | training: Barn, Caterpillar, Courthouse, Ignatius, Meetingroom, Truck | laser | F1 at the per-scene tau, `TanksAndTemples/python_toolbox/evaluation` with the provided alignment and crop |
| ETH3D high-res | the 13 training scenes | laser, metric | accuracy / completeness / F1 at 1, 2, 5, 10 cm, `ETH3D/multi-view-evaluation` |

- Downloads and evaluator repositories go in a data folder **outside** the
  repository; they are research-licensed and not redistributed.
- Tanks and Temples numbers depend on the protocol (see the Gaussian Wrapping
  paper). `results.csv` records the evaluator's commit in `protocol`; compare
  only against tables that used the same one.
- The published poses are used where the protocol does (DTU, Tanks and
  Temples), so the numbers isolate geometry from SfM. `-Sfm` runs Spirula's own
  SfM instead, reported as a separate variant suffix (`/spirula-sfm`).

`tools/meshlab/bench.ps1` runs, per scene: (given poses or `-Sfm`) →
`spirula dense` → `spirula geometry` (normals) → `spirula train meshing` →
the four variants (splat, splat + refine, dense, dense + refine) → a PLY in
the file frame → the official evaluator → rows appended to `results.csv`
(`dataset,scene,variant,metric,value,commit,upstream_commit,protocol`).
`-DryRun` prints every command, `-Reuse` keeps finished stages.

Expected: refine improves the splat baseline on DTU Chamfer and ETH3D F1, and
dense + refine leads at ETH3D's tight tolerances (1-2 cm).

### Measurement accuracy

On ETH3D's and DTU's metric ground truth: strip the scale (normalize the
reconstruction), restore it with `spirula scale` from ONE long ground-truth
distance picked from the GT points, then check `mesh-eval --measurements`
against several other GT distances spread across the scene. Report the scale
error and the per-distance error in mm or cm. Anyone can re-run it on public data.

## Tests

`src/mesh/tests/` and `src/data/tests/`, built with every backend:

| Test | Holds |
|---|---|
| `maxflow_test` | 6000 random small graphs equal brute-force min cut; the returned labelling cuts exactly that much; a long chain and a grid exercise adoption |
| `graphcut_mesh_test` | a sphere and a cube, randomly sampled and jittered by 2% of the spacing, seen by 40 synthetic cameras: watertight, one piece, Chamfer under half the point spacing. Exactly cospherical or coplanar samples are the Delaunay's degenerate case (`docs/notes/delaunay-degeneracy.md`): it overlaps tets there, which real dense clouds never are |
| `normal_refine_test` | a noisy cube with exact synthetic normal PNGs: noise RMS down at least 5x, creases within 2 degrees of 90, no shift past the cap |
| `rescale_test` | COLMAP and transforms.json (with `applied_transform`) datasets: cameras, sparse, dense, a splat model through its run's frame, a mesh and gauge.txt all scaled consistently, pixels unchanged, distances times `s` |

## GUI

The mesh panel (`GuiApp.cpp`, `draw_mesh_options`) gets a source selector
(Gaussians / dense cloud), "Refine with normals" and a max-shift field. They
are `MeshJob` fields saved by `MeshPreset.cpp`'s table, and `MeshRunner`
launches `spirula mesh-dense` / `spirula mesh-refine` the way it launches
`spirula mesh`. Strings are `msg::meshlab::gui_*`.

`mesh-dense` is CPU-only and takes no `--device`: the runner swaps in its argv
after the device flag is added, and the swap keeps only the executable and `--lang`.

### The ruler

`src/app/gui/MeasureTool.{h,cpp}`, a `ViewportInteractor` on the trainer
screen's 3D view, behind the **Measure** box on its header row (`GuiApp.h`: one
member; `GuiApp.cpp`: the toolbar call). Two clicks pick points through
`ViewportPanel::request_pick` (the preview's point pick, or the engine's depth
under the pixel); a third starts over. Picks come back in the shared frame,
are taken to the panel's model frame (the normalized one) and from there to
the file frame, `file = raw_to_file (train_to_normalized n + center)`, so the
distance is in dataset units, or metres once `gauge.txt` says `metric 1`.

**Set true length** asks first, then runs `rescale_dataset(dir, true / measured)`
off the UI thread, only while the dataset is parsed and nothing trains, and
re-parses it afterwards. The normalized frame does not change under a uniform
scale, so the two points stay where they were and now read the true length.
