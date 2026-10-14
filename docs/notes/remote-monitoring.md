# Watching a headless run from somewhere else

The whole GUI, not only a view of one run, can be used the same way:
[remote-gui.md](remote-gui.md).

A cloud GPU has no screen. Training has always served a browser viewer
(`src/app/webviewer/`); a reconstruction now does too, and both can be asked
questions by a coding agent through MCP. Everything here goes through
`HttpServer` (`src/app/webviewer/HttpServer.h`), the one HTTP server in the
tree.

## The short version

On the GPU box:

```bash
spirula sfm auto images/ -o ws/ --monitor 7008
spirula train --data ws/ --viewer-host 127.0.0.1 --viewer-token "$(openssl rand -hex 16)"
```

`sfm auto --monitor` prints an address with its token,
`http://127.0.0.1:7008/?token=…`. On the laptop:

```bash
ssh -N -L 7008:localhost:7008 -L 7007:localhost:7007 user@gpu-box
```

then open the printed address in a browser. `localhost` on the laptop is now
the box's loopback, so the URL works unchanged.

A run that is already going (another process gave it `--progress-dir`), or a
workspace copied back after it finished, is served by

```bash
spirula sfm monitor ws/ --port 7008
```

until Ctrl-C. It reads `ws/.progress` when there is one, or the directory it
is given.

## Why loopback and a token

The SfM monitor listens on `127.0.0.1` unless `--monitor-host` says
otherwise, and always behind a token: random unless `--monitor-token` gives
one. A cloud box's public address is reachable by everyone, and an SSH tunnel
costs one command. Binding `0.0.0.0` still works, for a trusted network, with
the token as the only guard.

The training viewer keeps its old default, `0.0.0.0` without a token, because
scripts depend on it; `--viewer-host` and `--viewer-token` close it. Both are
ordinary training flags (`src/config/TrainConfig.h`), so they reach the GUI's
options and `config.json` like any other -- including the token, which is
therefore not a secret from anyone who can read the run's output folder.

With a token set, a request without `token=<t>` gets a 403, and responses no
longer carry `Access-Control-Allow-Origin: *`: a page on another site can then
neither read the server nor drive it (the viewer's `/pause-toggle` is a GET
with an effect). Without one the viewer behaves as it always has. The GUI's
automation surface (`docs/notes/gui-automation.md`) uses the same check.

## What the SfM monitor serves

`src/app/SfmMonitor.cpp`. Every route wants `token=`. All of it except `/log`
is read from the files the run already writes for the GUI
(`src/sfm/core/Progress.h`, read by `src/app/SfmProgress.h`), so the monitor
costs the run nothing it was not already paying for once `--progress-dir` is
on.

| route | what |
|---|---|
| `/` | the page, `src/app/sfm_monitor.html`, embedded at build time |
| `/status` | JSON: `stage` (`sfm::Stage`), `done`/`total`, `registered`, `images`, `points`, `models`, `mean_reproj`, `finished`, `partial`, `metric`; `stamps` (one string per file, changing when it does), `log` (the next line number), `live` |
| `/models` | JSON `{"sizes": [...]}`: images placed in `model.bin`, `model_1.bin`, ... |
| `/model?k=` | model `k`, binary, below |
| `/pairs` | the match map, binary, below; `pairs.bin`, or the workspace's `matches.bin` once that is gone |
| `/images` | JSON: per image `name`, `placed`, `keypoints`, `points`, `mean`/`max` error, and `bad_err`/`bad_pts` in 0..1 on the GUI's scale; `images.bin`, or the workspace's `image_stats.bin` |
| `/log?since=N` | JSON: up to 2000 lines numbered `N` and on, `next` to ask for after them; only while `live` |
| `/progressive` | `progressive.txt` from the workspace, as text |
| `/thumb?name=` | the extractor's thumbnail of one image (`thumbs/`), JPEG; a name that climbs out (`..`), is absolute or names a drive is refused |

The page polls `/status` and `/log` every second and fetches the rest only
when its stamp moves, at most every three seconds for `/pairs`.

### `/model`

Little-endian. A 12-word header: `"SSMV"`, version 1, cameras, points, images
in the capture, flags (1 oriented, 2 metric, 4 provisional, 8 up given), the
scale (model units per normalized unit), the up axis (3 x f32; the mean camera
up when the model is not oriented), two words reserved. Then 23 words per
camera: `u32` image id, `f32` c2w 3x4 row-major in OpenGL axes, `fx fy cx cy w
h`, badness by error and by points (-1 when the image has no statistics), mean
error, 3D points. Then `f32` xyz per point and `u8` rgb per point.

Cameras and points are in the normalized frame the GUI's preview uses: centred
on the cameras and scaled to fit the unit sphere.

### `/pairs`

`"SSPM"`, `u32` images, bins, peak, staged; then `bins*bins` `u32` inlier
counts, and when staged the planned and the verified pair counts per cell, the
same three planes as `pairs.bin`.

## The log

`sfm/core/Log.h` hands every line to a tap (`slog::set_tap`) after the terminal
or the GUI's sink has it, formatted by `slog::line()` exactly as the terminal
prints it. The monitor keeps the last 5000. Nothing else holds a log -- a
child's stdout belongs to whoever started it -- so `sfm monitor` on an
existing workspace shows none, and says so.

## MCP

`tools/monitor_mcp.py`, standard library only, registered as
`spirula-monitor` in `.mcp.json`. It shares its stdio loop with
`tools/gui_mcp.py` (`tools/mcp_stdio.py`). It reads

- `SS_MONITOR_URL` (default `http://127.0.0.1:7008`), `SS_MONITOR_TOKEN`
- `SS_VIEWER_URL` (default `http://127.0.0.1:7007`), `SS_VIEWER_TOKEN`

so over the tunnel above it needs only the two tokens.

| tool | from |
|---|---|
| `sfm_status` | `/status`, with the stage named |
| `sfm_log` | `/log`: the last `tail` lines, or on from `since`; `level`, `grep` |
| `sfm_images` | `/images`: the `worst` N by error or by points, or the unplaced |
| `sfm_models` | `/models` |
| `sfm_progressive` | `/progressive` |
| `sfm_match_summary` | `/pairs`, reduced: pairs planned/verified, the peak, the weakest-linked images |
| `train_progress` | the viewer's `/progress` |
| `train_render` | the viewer's `/render`, returned as an image: orbit by yaw/pitch/distance or a raw c2w |
| `train_pause_toggle` | the viewer's `/pause-toggle` |

To check it by hand:

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"sfm_status","arguments":{}}}' \
  | SS_MONITOR_TOKEN=<token> python3 tools/monitor_mcp.py
```
