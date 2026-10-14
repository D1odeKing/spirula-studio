# Scale breaks along a sequence (D80)

## What goes wrong

Nothing in a run without GPS fixes the model's size; every part of it takes
its scale from the part it was registered against. A sequence -- a video's
frames, a walk -- is usually grown frame by frame along itself, each frame
registered against the points its neighbours triangulated (D79). If one of
those registrations puts a frame too close to its neighbour, the points it
then triangulates come out too near, the next frame registers against them,
and so does the rest of the walk. The model explains all of it perfectly;
it is just drawn at the wrong size, usually as a small copy sitting inside
the scene.

Seen on an Insta360 X5 capture (four videos, 6512 images, no GPS): video 462
joined the model last, near its end, and grew backwards one frame at a time
with no other image joining. After a pause in that growth it resumed with one
weak registration (341 of 1016 inliers), and the 400 frames from there to the
start of the video came out at **1/9** of their size. The camera's step per
frame was 0.15-0.2x the other videos' up to frame 9564 and 1.6x from frame
9598 on: a single step, not drift. The global bundle adjustment (every 10% of
growth, `--ba-growth`) ran after the break, by which time nothing tied the
small part to anything else. The duplicate-structure check saw cameras placed
together that shared nothing, but that check is for two places drawn on top of
each other, and it kept the model whole.

## The check

`findSequenceScaleBreaks` (`src/sfm/map/SequenceScale.h`) walks each
sequence's registered positions in order and compares two things on either
side of each position:

- **the step**: how far the camera moves per position (median over 8);
- **the depth**: the median distance from the camera to the points it
  observes (median over 8, a few positions back from the break, since depth
  changes over the frames that still see the other side's points).

A break is where both change by more than `--seq-scale-break` (3x for the
step, 2x for the depth) in the same direction. A walker who stops changes the
step but not how far away the scene is; a wall drawing nearer changes the
depth but not the step. A run of nearby positions that clear the bar is one
break, placed where the steps split best into two levels. An unregistered gap
longer than the window splits the walk: no scale is carried across it.

Of the two sides, the one registered later (by the mapper's registration
order) is the one built on the break. Its images, from the break outward for
as long as they registered nearer that side's time than the other's, are what
gets undone; a rig's other lenses go with them.

## The repair

In `Mapper::growLoop`, every `--seq-scale-check-every` registrations (50), over
the sequences that took a registration since the last check, and once more over
every sequence at the end of `Mapper::grow`:

1. The frames built on the break are de-registered and their trial budget
   reset, so growth offers them again.
2. They re-register against the whole model's points: D79's preference for
   the pose the neighbours' points support is skipped for them.
3. A break found again at the same place (within two windows) leaves those
   frames out of this model instead of regrowing them a second time.

The check reads the poses as they are; it adds no bundle adjustment unless it
undoes something, and the growth that follows pays for the rest.

```
[map] scale break along VID_..._462/cam0 at position 398: steps x0.11 and depth x0.12 past it; 32 image(s) undone to regrow
[map] scale breaks along sequences: 1 found, 32 image(s) undone to regrow, 0 left out
```

`--seq-scale-break 0` turns it off.

## Not covered

- Breaks made by a merge of two models (`sfm/map/Merge.h`) rather than by
  growth: the check runs in the growth loop only.
- Images outside any sequence: without an order there is no step to compare.
- A whole sequence at the wrong scale with no break inside it -- one video
  attached to the rest through a single weak link. The step is then even
  throughout; only a metric reference (GPS, or the accelerometer through
  `--sensor-map`) can tell.

`src/sfm/tests/sfm_sequence_scale_test.cpp` covers the check on synthetic
walks: a 1/9 stretch either side of the break, a long stop, a wall drawing
near, an uneven pace, a two-lens rig, a short side and a gap.
