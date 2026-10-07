# Rig constraints in the mapper and BA — a survey

Written 2026-09-08 while adding GoPro `.360` support (`docs/datasets.md`, "360
cameras"). **Implemented 2026-09-10 as item 4 below, with items 1 and 2 on top
of it**: `src/sfm/core/Rig.h` is the definition, `src/sfm/README.md` "Rigs"
what the run does with it, `src/sfm/ba/README.md` "Rigs" the solver layout.
Item 2's registration became a generalized-camera PnP on 2026-09-17, and frame
completion a joint estimate with it — the last two sections of this file, and
the answer to the line in item 4 that says the mapper's PnP would want one.
The survey is kept as written; the question it answered was what it would take
to tell the reconstruction that a set of images has a fixed — possibly
optimizable — relative pose, and whether that would be a better answer than the
current one for a 360 capture.

`src/sfm/README.md` lists rig constraints as deliberately out of scope, and
`docs/notes/sfm-in-process-plan.md` §4 already says where they would arrive
from (a `sfm::Manifest` carrying rig definitions, instead of the
directory-of-JPEGs-plus-prefix-flags interchange we have). This note is about
the other half: what would consume them.

## Why it comes up

A `.360` unwraps into ten perspective views per frame. Within one lens, the
five views share an optical centre **exactly** and their relative rotations are
ours by construction — they are a pure-rotation rig with no unknowns at all.
Between the two lenses there is one unknown 6-DOF (about 3 cm of baseline and a
180-degree turn), constant for the whole capture and, for that matter, for the
camera model.

The reconstruction is told none of this. Every one of the ten views is
registered on its own, and views of the same frame share no features with each
other — they only meet at their edges — so what holds them together is pairs
across *time*. On a 78-second handheld walk at 2 fps this reconstructs, but as
several components (measured: 645 of 930 images over 3 components, 0.84 px);
the pieces are usually different views of the same frames, which a rig would
have fused for free.

The same shape of problem is the reason a single equirectangular image per
frame "aligns more easily": it is one camera, so the connection between what
the six directions see is not something the mapper has to discover.

## What the code assumes today

| | |
|---|---|
| pose parameterization | 6 per image, angle-axis + t, `map/Bundle.h:180`; the reduced camera system's columns are `6 * image_index` (`ba/Problem.h`, `pose_dim = 6 * num_images`) |
| intrinsics | per *group*, shared across images, with a free/fixed split (`Group::n_intr`, D50) — the one place sharing already exists |
| points | Schur-eliminated; the reduced system is camera-side only |
| linear solves | dense-by-observation, dense-by-pair, or matrix-free PCG (`ba/Solver.h`). The pair path needs `exclusiveGroups()`: **every column of S owned by at most one image pair** |
| preconditioner | one block per image over `[pose | intrinsics]`, or a split partition when a group is shared (`buildPrecBlocks`) |
| registration | 2D-3D PnP RANSAC per image, then nonlinear refinement and inlier gates (`map/Mapper.h`) |
| merging | Sim3 from **shared images** only (`map/Merge.h`); two models sharing no image cannot be related — and `src/sfm/README.md` lists exactly that as out of scope |
| interchange | path-prefix flags: `--camera-model cam0=pinhole`, `--focal cam0=752`. A rig is not expressible |

The one structural fact that matters most: **poses are per image and their
columns are indexed arithmetically**. Intrinsics sharing was designed in;
pose sharing was not.

## Four places a rig could bite, cheapest first

### 1. Rig-aware model merging (no BA change)

`alignReconstructions()` needs shared images to get a Sim3. Given a rig, two
models that share *no* image can still be related: if model A holds
`cam0/00042` and model B holds `cam3/00042`, and the rig says how those two
views sit relative to each other, that is a pose correspondence — and a handful
of them across the overlap gives the same Sim3 the shared-image path computes,
by the same RANSAC over predicted centres.

This is the smallest change with the largest effect on what a `.360` capture
currently produces, because the components it fails to merge are usually
different views of the *same frames*. It needs: a rig table, a way to ask "what
is the rig-mate of this image in that model", and one more candidate-pair
source in `MergeSession`. It touches no kernel and no parameter layout.

**Effort: small.** A day or two, mostly in `map/Merge.h` and the plumbing to
carry the rig in.

### 2. Rig completion at registration (no BA change)

When PnP registers one view of a frame, the other nine follow from the rig
without any 2D-3D correspondence of their own. Today each has to earn its own
inliers, and a view pointing at sky or at the operator's chest never does — the
per-folder registration counts in the measured run are 88 to 153 out of 155
depending on which way the view points.

The mechanics are already there: `registerImage` sets a pose and then filters
and triangulates. A rig-completed pose would be seeded rather than solved, and
then subjected to the same gates (an image whose rig-implied pose has no
support is still refused). The risk is the obvious one — a wrong seed pose that
passes the gates poisons the model — which argues for admitting a completed
image only when it triangulates.

**Effort: small-to-medium.** Contained in `map/Mapper.h`.

### 3. Rig residuals as a soft prior in BA

Add a residual per rig edge: the deviation of `pose_b` from `rig_ab * pose_a`,
weighted by how much you trust the rig. This leaves the parameter layout alone
— poses stay per image, columns stay where they are — and only adds terms to
the reduced system.

The catch is *where* those terms land. The pair-Schur path's tables are built
from co-visibility (`pair_entries`, one entry per shared point), and two views
of one frame share no point, so the pair simply does not exist in the table.
A rig prior would have to inject pairs, and `exclusiveGroups()`/`buildPrecBlocks`
would need to keep holding. The PCG path is friendlier: it never forms S, so a
prior is one more contribution to the matrix-free product.

There is also no constant-parameter mask for poses (only for intrinsics), which
`docs/notes/sfm-port-plan.md` §9 already lists as missing and which a hard rig
would want.

**Effort: medium.** Host + Slang, both linear solvers, and a new residual type
in a solver that currently knows only reprojection.

### 4. Reparameterization — the real thing

`pose_image = pose_rig ⊕ extrinsic_member`, with the rig pose and the member
extrinsics as the parameters. Six columns per *frame* instead of per image, plus
six per rig member for the whole capture. This is what COLMAP 3.10's rig support
does and what a fully general "optimizable relative pose between arbitrary pairs"
means.

Everything in the table above moves: the column layout (`6 * i` is no longer the
address of an image's pose), the Jacobian blocks (an observation now
differentiates against two pose blocks, so `kMaxCamDof` and the `2 x dof` block
shape change), the pair-Schur ownership test, the preconditioner partition, the
Slang kernels in `sfm/shaders/ba/ba.slang`, and the host mirror in
`ba/SolverCpu.h`. The mapper's PnP would want a rig-aware variant
(generalized-camera absolute pose) to exploit it fully.

**Effort: large.** This is the one that is out of scope for a reason: it is a
rewrite of the solver's addressing, not a feature on top of it.

## What a `.360` rig would actually contain

Worth stating because it is much stronger than the general case:

- Within one lens, five views, **zero relative translation and known relative
  rotation**. No parameters at all. A rig prior here is a hard constraint that
  cannot be wrong.
- Between the two lenses, one 6-DOF, **constant for every frame of every
  capture from that camera model**. It could be calibrated once from a good
  reconstruction and then reused — or left as the single optimizable rig
  parameter, which is the smallest useful instance of item 3 or 4.

So a 360 capture does not need "arbitrary optimizable relative poses" to
benefit. Items 1 and 2, with a fixed known rig, would take the ten views of a
frame from ten independent guesses to one pose plus a table.

## The dual-fisheye caveat

`.insv` extraction runs `extract_track` **per track**,
and each track picks its own sharpest frame within a window
(`FrameExtract.cpp`). Two tracks can therefore keep frames a few tens of
milliseconds apart, and on a moving capture that is not a rig — it is a rig plus
an unknown, time-varying offset. Any rig work on dual-fisheye needs the paired
extraction the `.360` path already does (`extract_pair`, one window over both
tracks, one decoded index for both), or a recorded per-image timestamp so the
mapper can decline the constraint when the pair is not simultaneous.

The `.360` path is already rig-ready in this sense: the ten views of a frame
come from one decoded frame pair and carry the same file stem, so the rig group
is recoverable from the filename alone — which is exactly what item 1 needs and
is why it is cheap.

## The generalized PnP — what registration does now

Written 2026-09-17. The survey above left registration at item 2: a member's
own P3P proposes the frame's pose and the other members only get to vote on the
proposals. That is a rig-aware *selection*, not a rig-aware *estimator*, and it
has two holes. A lens with fewer than four correspondences proposes nothing,
however many the frame has between its ten; and each proposal is the winner of
a RANSAC that maximized **that lens's** inlier count, so a lens whose own
correspondences are half outliers hands over a pose that fits its own noise and
the right pose is never generated at all.

`geometry/AbsolutePose.h` `ransacRigPnP` replaces it with one LO-RANSAC over
every member's correspondences at once. A minimal sample is three of them
wherever they fall; scoring is the joint reprojection over all the members,
each residual divided by its own lens's inlier radius so lenses of different
focal length share one threshold; the local optimization refines the frame pose
over the joint inliers (`refineFramePose`). Three rays through one optical
centre are P3P, three that are not solve as a generalized camera:
`geometry/GP3P.h` `gp3p`, the non-central three-point absolute pose. That is
what makes the estimator generalized rather than merely rig-aware — a frame
whose lenses each hold one or two correspondences is still posed.

### gp3p, and why the elimination is the shape it is

Unknown depths λ_i along three rays with known origins o_i and directions d_i
in the rig's frame, constrained by the three world distances:

    |o_i + λ_i d_i − o_j − λ_j d_j|² = |X_i − X_j|²

Three quadratics in three unknowns, Bézout number 8 — the octic every published
gP3P ends at. The useful structure here is that **each equation touches only
two of the three unknowns**. Hiding λ₀ makes the first two monic quadratics in
λ₁ and λ₂; reducing the third by them leaves a *bilinear* relation, so λ₂ is a
ratio in λ₁, and one resultant of two quadratics closes it. No Gröbner basis,
no 3-quadratic solver, ~40 lines of polynomial arithmetic on degree-8 arrays.

Two numerical facts, both measured on 2000 random samples per configuration:

- The variable has to be rescaled by the geometric mean of the root magnitudes
  before the root finder runs. A depth is tens of times the triangle it spans,
  so the raw octic's coefficients span 10¹⁰ and an Aberth iteration started on
  the Cauchy circle never arrives: **7% of samples came back with no root**.
  With the rescale, 100%.
- Roots are retired one at a time rather than on the worst of them. A nearly
  central rig drives them together in pairs that trade their last bit back and
  forth forever: 45% of samples ran the whole iteration budget for nothing, and
  retiring them individually cut the solver from 23 µs to 7.5 µs.

Below a baseline of 10⁻⁵ of the triangle's size the rays count as concurrent and
`p3p` takes over. The octic is 100% reliable from 2·10⁻⁵ upward and the central
approximation is exact to well under a hundredth of a pixel below it, so the
switch costs nothing on either side; in between, an octic with roots that close
is not worth solving. A `.360` rig read from a manifest lands exactly on that
path, since the five views of one lens share an optical centre by construction.

### The sample stays inside one lens when it can

This is the part that is not obvious and that measurement settled. Sampling
uniformly from the pool means almost every sample spans lenses — with ten
members holding equal shares, only 1% of triples come from one lens — and a
sample that spans lenses carries the **calibration's** error into the
hypothesis. A rig estimated from the reconstruction (`Mapper::calibrateRigs`)
is good to a few tenths of a degree, which at f≈750 is several pixels: worse
than the inlier threshold the hypothesis is about to be scored at. Uniform
sampling measurably lost coverage on a `.360` capture.

So the lens the first draw landed on fills the sample whenever it holds three,
which weights a lens by how many correspondences it brought, and only a lens
too small to fill one reaches across. Three rays of one lens are exact whatever
the calibration is worth; the rig is then used where it is sound — in the
score, the refinement and the gates, all of which are joint.

### What it is worth

Paired measurement, both schemes run on the same frame inside the same run, so
the reconstruction's trajectory is held fixed:

| capture | frames | joint better | worse | inliers |
|---|---|---|---|---|
| `.360`, 10 views/frame, 400 features/image | 71 | 57 | 9 | 21673 → 22612 (+4.3%) |
| `.360`, 10 views/frame, 700 features/image | 70 | 58 | 10 | 42010 → 42874 (+2.1%) |
| `.insv` dual fisheye, 2 lenses, 235 frames | 176 | 160 | 15 | 188904 → 201961 (+6.9%) |

No frame lost more than 10% of its inliers in any of the three; 17 of the 71
gained more than 10%. The gain is largest where it was predicted: on the `.360`
run at 400 features, frames whose whole pool was under 200 correspondences came
out 9.1% better and won 19 times out of 20, while frames with a thousand
correspondences to spare gained 5%.

End to end the picture is noisier, and honestly so: on a deliberately starved
`.360` (the same 1000 images at 300 to 1200 features each, where the default is
8192) the run lands on 1000/1000 in one model either way at 300, 800, 1000,
1200 and at the default; the new estimator wins 700 (the old one splits into
four models and 755 images, the new one holds one model and 808) and loses 400,
500 and 600 (about 950 images in two models against 1000 in one). Those three
losses trace to `calibrateRigs` declining a member for exceeding
`--rig-max-spread`, a knife-edge threshold on a starved capture that better
poses move as readily as worse ones: with the threshold taken out of the
decision, 400 and 500 are 1000/1000 for both. At the settings anyone runs, the
two agree on coverage and the joint estimator carries slightly more
observations at slightly lower reprojection error.

### Completion is joint too, in two stages

`Mapper::completeFrame` (which replaced `registerFromRig`) does the other half:
a frame with a lens already placed used to place each remaining lens on its own,
refining the rig's prediction against that one lens's correspondences. Now the
prediction is refined **once**, over every waiting lens's correspondences
together, and every lens takes its pose from that one frame pose.

The first attempt stopped there, and it was worse — measurably. A rigid frame
pose cannot beat N independently refined poses on the sum of their own inliers,
because that sum is exactly what the per-lens scheme maximizes: joint-only came
out 7.3% down on the default capture, losing 21 of 25 frames. The reason is not
that joint estimation is wrong but that **the calibration is not exact**. At a
few tenths of a degree of `cam_from_rig` error, no single rigid pose fits every
lens at its own pixel threshold, and the per-lens refinement had been quietly
absorbing that error one lens at a time.

So there is a second stage. After the frame's pose is settled, a lens with a
workable set of its own refines on top of it, under the same movement bound as
before and accepted only when it explains **more** than the frame's pose did.
That cannot lose — the frame's pose is the floor — and it restores everything
the per-lens scheme had:

| capture | multi-lens completions | per-lens | joint only | joint + own |
|---|---|---|---|---|
| `.360`, default features | 25 | 34614 | 29158 | **35079** (+1.3%) |
| `.360`, 400 features | 74 | 4698 | 4322 | **4799** (+2.1%) |
| `.360`, 500 features | 23 | 2290 | 2201 | **2356** (+2.9%) |

(inliers over the frames where two or more lenses were waiting; the joint
stage does nothing when only one is, which is most completions on a two-lens
rig.)

What the joint stage buys is not those percent: it is the lenses that have
almost nothing of their own. They used to sit exactly on a prediction carried
from one other lens; now they sit on a pose the whole frame agreed to. End to
end on the starved `.360` sweep — the same 1000 images at 300 to 1200 features,
where the default is 8192 — that closes the gap the frame-PnP change had opened
and then some:

| max-features | before any of this | frame PnP only | + joint completion |
|---|---|---|---|
| 300 | 1000 / 1 model | 1000 / 1 | 1000 / 1 |
| 400 | 1000 / 1 | 945 / 2 | 1000 / 1 |
| 500 | 1000 / 1 | 1000 / 1 | 1000 / 1 |
| 600 | 1000 / 1 | 963 / 2 | 1000 / 1 |
| 700 | **755 / 4** | 808 / 1 | **1000 / 1** |
| 800 / 1000 / 1200 / default | 1000 / 1 | 1000 / 1 | 1000 / 1 |
| `.insv` dual fisheye | 470 / 1 | 470 / 1 | 470 / 1 |

Every budget now reconstructs whole in one model, including the one where the
old code came apart into four.

## Known lens geometry, and matching

Written 2026-09-19. Everything above estimates a rig from the reconstruction:
a member is only used once enough frames registered it on its own, and then
held to within `--rig-max-spread` of that estimate. For the cameras that
produce most rigs, the geometry is known before the first image is read, and
two more things follow from knowing it: a tighter parameterization, and a say
in which pairs get matched.

### What the cameras are

Calibrated `cam1_from_cam0` of three dual-fisheye cameras, against the
nominal "back to back, turned 180 degrees about the image's vertical"
(`dualFisheyeNominal`, `Rig.h`):

| camera | source | rotation off nominal | translation (lateral, axial) |
|---|---|---|---|
| PortalCam, fisheyes 0 and 1 | the device's own sparse model, 856 frames | 0.83 deg | 0.4 mm, 87.7 mm |
| DJI Osmo 360 | this pipeline, metric via the IMU, 36 frames | 1.4 deg | 0.9 mm, 26.3 mm |
| Insta360 X5 | this pipeline, two captures | 0.8-1.0 deg | see below |

The rotation is right to a degree -- the lenses are mounted, not machined to
the axis -- which at a fisheye focal of ~550 px is 8-12 px: good as a start,
wrong as a constraint. So `kind: dual-fisheye` starts the rotation there and
refines it. The translation is the other way round: the baseline lies on the
optical axis to under a millimetre on both cameras measured against something
metric, so it could be one parameter, `t.z` in the second lens's frame (`refine:
axial`, `BAProblem::Member::mask` 0x27). `kind: dual-fisheye` imposes that on
the second member.

The Insta360 is the open case. Refined freely, its lateral component comes
out at 30-50% of the axial one on both captures (`t_y` 0.0019 against
`t_z` -0.0035). Held to the axis on the same pairs, the model has more points
and lower error: 194103 points at 0.914 px against 178478 at 0.966 px on a
470-image walk. A physical offset would do the opposite, so the lateral term
is more likely absorbing something that is not a rigid offset (the two
sensors read out in different directions), which is why the axis is the
default.

A GoPro `.360`'s views are cut from one canvas at rotations the extraction
chose (`app/Pano360.h`), and the five of one lens share its centre. The rig
calibrated from a 2090-image capture agrees with those rotations to 0.12-0.46
deg, and part of that residual is real: held at the cut rotations, the same
capture ends at 0.785 px; started there and refined, at 0.679 px, level with
the estimated rig (0.682 px, 321551 points against 322327). So Spirula Studio
starts every view at its cut rotation and refines it -- `refine: rotation` for
the views of the reference lens, whose translation is exactly zero, `all` for
the other lens's.

Two things surfaced on the way, both general. `rotationToAngleAxis` lost the
axis of a rotation within ~1e-6 rad of 180 degrees -- the skew-part formula
divides rounding noise by the sine -- and returned roughly the identity; two of
the other lens's views sit at exactly 180 degrees about a tilted axis, so they
came out turned half a revolution. It now goes through the quaternion
(`core/Pose.h`, and a round trip in `sfm_geometry_test`). And a known member
is established before it has observed anything, so a view on the sky could
enter a solve refined on next to nothing; `BundleOptions::rig_min_obs` (100)
holds a member with fewer observations in the problem than that.

### What knowing it buys

- **The rig from the seed.** An established member is used by the first
  registration, so frames register as frames from the start instead of lens
  by lens until `calibrateRigs` has three frames to average.
- **No early estimate to get wrong.** The estimated calibration is taken from
  the first three to five frames that registered both lenses, and on a
  1048-image Osmo 360 walk that estimate is where the run is decided: once it
  declined the second lens as unsynchronized (1.76 deg spread) before taking
  it, and in another run of the same frames it accepted one from four frames
  with a translation a hundred times the real baseline, after which the rig
  placed 251 images on its word alone and the model ended with 251489 points
  at 1.85 px. A known member is never estimated early, and never declined.

The same walk, one change at a time (`--quality medium`, 1048 images):

| run | registered | points | error | mapping |
|---|---|---|---|---|
| estimated rig, old pairing | 1046 | 359341 | 1.52 px | 3:39 |
| `dual-fisheye`, old pairing | 1042 | 401979 | 1.47 px | 3:38 |
| estimated rig, new pairing | 1040 | 251489 | 1.85 px | 4:31 |
| `dual-fisheye`, new pairing | 1040 | 401718 | 1.47 px | 5:15 |

The last row spent its extra mapping time on 24% more correspondences and on
seeding sub-models around four frames looking straight down at a paved slope
from close range, which the first row had registered lens by lens at 6-20%
inlier ratios.

### Rig-mates in matching

With the member rotations known, a verified pair says more than that two
images overlap: if `cam0/i` matches `cam0/j`, frames i and j face the same
way, so `cam1/i` faces `cam1/j`; if `cam0/i` matches `cam1/j`, the camera
turned round between them and `cam1/i` faces `cam0/j`. On a dual fisheye the
two lenses see disjoint halves of the scene, so content-based pair selection
ranking one lens's pair says nothing about the other's -- and a frame link
resting on one lens is exactly the weak link a split walk breaks at.
`--rig-pairs` (`feature/RigPairs.h`) matches those rig-mates in a second
verification pass, seeded by pairs that verified with
`--rig-pair-min-inliers` (30), for frame pairs the first pass joined with at
most a quarter of the rig's members.

How it was arrived at, on the dual fisheyes and the `.360`:

| variant | capture | mates tried | verified |
|---|---|---|---|
| mates of every shortlist pair, before verification | PortalCam, 1712 images | 12333 | ~25%, 4x the matching time |
| mates of verified pairs | PortalCam | 7953 | 72% |
| mates of verified pairs | Osmo 360 walk, 1048 images | 4235 | 95% |
| mates of verified pairs | Insta360 X5 at 400 features | 406 | 62% |
| mates of verified pairs, weak frame links only | `.360`, 10 views, 700 features | 9479 | 8% |
| ... and only lenses facing away from the seed's | same | 5622 | 6% |

So it applies to `kind: dual-fisheye` rigs alone. "The seed pair faced alike"
is true to within the lenses' overlap for two ~190-degree fisheyes; two 90-degree
views overlap at 60 degrees apart, and their counterparts then do not. A `.360`
would need each seed's measured relative rotation rather than the assumption:
the verifier's inliers give it for the price of an essential decomposition, and
a mate could then be verified with a 2-point translation RANSAC and gated on
agreeing with its seed. That is the same machinery the telemetry proposals need
for gyro-predicted rotations, and it is not built.
