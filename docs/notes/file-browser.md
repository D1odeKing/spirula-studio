# The Files window

**File > Browse Files...** (or **Files** on the home screen) opens a window
over whatever screen is up. It lists every storage the machine has mounted,
lets you walk its folders and look inside a file, and moves files between the
machine and the computer you are sitting at. It is a window of its own: a
reconstruction or a training run keeps going behind it, and nothing it does
touches the engine.

On a cloud GPU (`spirula gui --remote`, [remote-gui.md](remote-gui.md)) "the
machine" is the pod and "your computer" is the one with the browser. That is
the case it was built for: a pod's network volume, its container disk and
`/dev/shm` all show up as locations, and the files on them can be checked
without an SSH shell.

## What it shows

| part | |
|---|---|
| Locations | Every mounted file system with its free space, root first, then home. Pseudo file systems (proc, sysfs, cgroup, overlay internals, Docker's bind-mounted `/etc/hosts` and friends) are left out; `/dev/shm` is kept. Windows lists its drive letters, macOS `/Volumes`. |
| Toolbar | Back, Up, Refresh, an editable path, a name filter and **Hidden files**. |
| Table | Folders first, then files, sortable by name, size, modified time and kind. Read on a worker thread, since a folder on network storage can take seconds to stat. |
| Preview | Depends on the kind, below. |

| kind | by extension | preview |
|---|---|---|
| Image | jpg, png, bmp, tga, tif, webp, exr, heic, ... | the picture |
| Video | mp4, mov, mkv, insv, 360, ... | frames, through the dataset screen's player; **Play in browser** on a remote session |
| Text | txt, json, yaml, csv, log, md, sh, py, ... | the first 1 MB, read-only (a NUL or bad UTF-8 means "not a text file") |
| Splat / point cloud / mesh | ply (told apart by its header), splat, spz, e57, las, obj, ... | the PLY header: splat count and SH degree, or point/vertex/face counts, and the properties; **Show points** draws a cloud |
| Folder of photos | | **Show photos**, the dataset screen's grid |

A video uses the GPU decoder only while no run is using the device and no
editor is open (the dataset screen's own rule); otherwise it decodes with
ffmpeg, so looking at a video never competes with training.

## Opening splats and clouds in the Spirula Viewer

**Open in Spirula Viewer** never loads the file into the running app.

- **Remote**: the page opens a new tab with the web viewer (`/viewer/`, the
  same `viewer/` bundle the repo ships, embedded in the binary), which streams
  the PLY from `/remote/raw/` and draws it with the laptop's GPU. The pod only
  serves bytes. Browsers allow a new tab only after a click, so the page shows
  a banner with an **Open** button.
- **Local**: a second, detached `spirula` process opens it in a window of its
  own (`spawn_detached`). Closing either window leaves the other alone.

**Open in Spirula Studio** does what dropping the file on the window does.

## Moving files

| | remote session | local |
|---|---|---|
| to your computer | **Download**: a file as itself, a folder as one `.tar` streamed as it is read (no temporary copy) | **Export...**: copy into a folder you pick |
| from your computer | **Upload here...**: files or a whole folder, from the page's panel, or dragged anywhere onto the page while the Files window is open | **Import files...** / **Import folder...** |

An upload lands in the folder the Files window shows. Each file is written as
`<name>.part-<random>` and renamed into place once complete, so a cut-off
upload never leaves a file that looks whole. An existing file is not replaced
unless you confirm it. Names from the browser are relative; an absolute name
or one with `..` is refused.

## Routes

Added to the remote GUI's `HttpServer` by `gui::remote::add_file_routes`
(`src/app/gui/RemoteFiles.h`). All of them need the session token, given as
`?token=` or as the `ss_token_<port>` cookie the page sets (HttpOnly,
SameSite=Strict), so the viewer tab and `<video>` work without the token in
every URL.

| route | method | |
|---|---|---|
| `/viewer/<file>` | GET | the embedded web viewer; `index.html?model=<url>` loads a PLY |
| `/remote/raw/<name>?path=P` | GET | the file at P, inline, with its content type; honours `Range` (206), which `<video>` seeking and the viewer need. `<name>` is only there so the URL ends in the file's name. |
| `/remote/download/<name>?path=P` | GET | P as an attachment; a folder as a `.tar` (ustar, GNU long names) |
| `/remote/upload?dir=D&name=N[&overwrite=1]` | POST | the body written to D/N; 409 if it exists, 400 if the body was shorter than `Content-Length` |

`/remote/status` also carries the page events the Files window raises
(`viewer`, `download`, `upload`, `play`) and `drop`, the folder a drag onto the
page goes to; the page polls it with `events=<last seen>`.

## Code

| file | |
|---|---|
| `src/app/gui/Storage.{h,cpp}` | mounts, listing, kinds, PLY header probe, text read; no GUI |
| `src/app/gui/TarWriter.{h,cpp}` | streaming tar of a folder |
| `src/app/gui/RemoteFiles.{h,cpp}` | the routes above |
| `src/app/gui/FileBrowser.{h,cpp}` | the window |
| `src/app/gui/remote_gui.html` | the page's upload panel, drop zone, player and viewer banner |
| `src/app/gui/tests/files_test.cpp` | all but the window, routes over a real socket |
