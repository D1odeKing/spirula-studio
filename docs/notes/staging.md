# Working on a local copy, and masking folders in parallel

## Opening anything from a network volume (`src/app/gui/LocalCopies.h`)

Opening a large dataset straight off a pod's volume stalls the whole GUI --
and with it the remote page -- while thousands of files are read one round
trip at a time. So whatever is opened from a network volume is copied first,
and opened from the copy:

- **Every way in**: a drop, a command-line argument, the Files window's "Open
  in Spirula Studio", a recent entry, and every file picker that opens or reads
  something (datasets, photos, videos, models, scans, runs to resume). Pickers
  that name somewhere to *write* are left alone.
- **The mirror**: `/mnt/vol/a/b` is copied to `<disk>/spirula-local/mnt/vol/a/b`
  (`<disk>` is `/data` unless `local_copies_dir` is set). A folder's top-level
  `outputs/` stays on the volume until it is opened itself. 16 files at a time;
  a file of 256 MB or more in 64 MB pieces, 8 at once.
- **While it copies** a window shows the file and byte counts and the rate,
  with **Cancel** and **Open on the volume instead**; the GUI keeps drawing. If
  the disk has no room, or the copy fails, it opens on the volume and says so.
- **Back to the volume**: every 30 seconds, from **File > Send local changes
  back now**, and when the program quits, what is new, changed or deleted in
  the mirror is done on the volume. Changes are found by listing the local
  disk against the record of what the volume has (`.spirula-local-index`), so
  the volume is not asked about every file. Only places under a folder that
  was opened (and beside it) are written back.
- **Remembered as the real thing**: recent entries and picker folders name the
  volume path, not the copy.
- **File > Work on local copies of network files** turns it off.

Checked on an sshfs mount: opening a 400-photo folder copied 400 files
(104 MB) and opened the dataset screen on the copy; a file written into the
dataset folder beside it was on the volume 30 seconds later.


Two things a dataset run on a cloud GPU needs that a desktop does not, taken
from what `cloud_commands.py` scripts by hand on RunPod.

## Staging (`src/app/gui/Staging.h`, `SfmRunner::stage_in` / `stage_out`)

A pod's network volume is object storage behind FUSE: reading or writing one
file costs a round trip, so a run that writes thousands of frames, masks and
feature files there spends its time waiting, while the same files copied 16 at
a time move at the network's full speed.

With **Settings > Advanced > Work on a local copy when on a network volume**
(on by default; saved with the dataset as `sfm_stage`, `sfm_stage_dir`):

1. The run looks up the file system the dataset folder is on
   (`/proc/self/mounts`). A FUSE mount (`fuse.*`), NFS, SMB/CIFS, 9p, Ceph,
   Gluster, Lustre, GPFS, AFS or virtiofs counts as a network volume; anything
   else runs in place, as before.
2. It picks the local disk: the folder set in **Local disk**, or `/data`. The
   disk must exist, not be a network volume itself, and have room for the
   dataset plus half again plus 2 GB. If not, the log says why and the run
   works on the volume.
3. It copies the dataset folder to `<disk>/<name>-<hash of its path>`, 16 files
   at a time. A file already there at the same size and time is skipped, so the
   copy a previous run left makes the next one quick. A file deleted on the
   volume since is deleted from the copy. Left on the volume and read in place:
   the run's inputs (videos, photo folders, masks that came with them, even
   when they are inside the dataset folder) and `outputs/`.
4. The run works on the copy. The copy-in shows as "Copying the dataset to the
   local disk" on the Frames step, with a file count.
5. When the run ends -- done, failed or cancelled -- what changed is copied
   back, 16 at a time, and a file the run deleted that it had copied in is
   deleted from the volume. Nothing it never saw is touched. Absolute paths to
   the copy in small text files (`.json .txt .yaml .yml .cfg .ini .csv`) are
   rewritten to the volume first, and the paths the screen shows afterwards
   are the volume's.

Files are written as `<name>.stage-part` and renamed, so an interrupted copy
never leaves a file that passes the size check. Copies keep their source's
modification time, which is what makes "same size and time" mean unchanged.

`.resume/*.sig` keeps the copy's path: every staged run of a dataset uses the
same copy, so they match; a run with staging off re-extracts features once.

Measured on an sshfs mount (`fuse.sshfs`) standing in for a pod volume: a
20-photo dataset ran on `/data/photos_dataset-<hash>`, placed 20 of 20, and
76 files (34.6 MB) went back; a second run reused everything and copied back
one file.

Training is not staged yet: it reads the dataset where it is.

## Parallel masks (`src/app/gui/MaskWorker.h`, `DatasetPrep::mask_in_workers`)

The neural-net layer records every GPU dispatch into one process-wide stream,
so two maskers in one process would interleave their work. Each camera folder
(each lens of each video, each photo folder) is instead masked by a child of
the same executable:

```
spirula gui --mask-worker <dataset>/.progress/mask-workers/folder-N.json
```

The job file carries the mask settings, the device, the colour settings and
the input with its stencil (drawn shapes as text, the fitted border per
camera). The child runs the same `generate_masks` the in-process path does and
prints `@@progress DONE TOTAL`, `@@log 0|1 TEXT`, `@@error TEXT` and, last,
`@@folded 0|1`. The parent adds the progress of all streams into the Masks bar,
prefixes each log line with its folder, and stops the others on a failure or a
cancel.

**Masks > Advanced > Folders at once** (`mask_streams`): 0 picks free GPU memory
/ 3 GB, at most 8, from `nvidia-smi`; where that cannot be read (no NVIDIA GPU)
it is 1. 1 masks folders one after another in-process, exactly as before. The
live film strip of masks is not fed by the workers.
