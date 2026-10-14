# The whole GUI on a cloud GPU, in a browser

`spirula gui --remote PORT` runs the desktop app on the machine with the GPU
and shows it in a browser on another one. The window never appears on the GPU
box: each frame that changed is sent to the page as a JPEG, and the page sends
its mouse, keyboard, clipboard and size back. Every path picked, every setting
saved and every run started is on the GPU box, which is where the data, the
models and the outputs live.

It is the remote half of [remote-monitoring.md](remote-monitoring.md): the
same `HttpServer`, the same loopback address and token, the same SSH tunnel.

## The short version

On the GPU box:

```bash
spirula gui --remote 7010
```

It prints `Remote GUI: http://127.0.0.1:7010/?token=…`. On the laptop:

```bash
ssh -N -L 7010:127.0.0.1:7010 -p <ssh port> root@<host>
```

then open the printed address there. The GUI's window follows the browser
window's size; close the tab and the GUI keeps running, open it again and it
is where it was.

On a box with no display (every cloud GPU), `--remote` starts an Xvfb of its
own and draws into it with Mesa's software OpenGL. That draws only the
interface: the viewport's pictures come from CUDA or Vulkan as pixels, never
through GL, so the GPU is untouched by it.

| flag | default | |
|---|---|---|
| `--remote PORT` | | serve the GUI on PORT; `SS_GUI_REMOTE=PORT` does the same |
| `--remote-host HOST` | `127.0.0.1` | `0.0.0.0` opens it to the network, guarded only by the token |
| `--remote-token TOKEN` | random | what `?token=` must carry |
| `--remote-quality Q` | 80 | JPEG quality; 60 halves the bytes on a slow link |
| `--remote-fps N` | 30 | most frames a second sent |

## What it needs on the box

- A build with `SS_BUILD_GUI=ON` (GLFW, Dear ImGui; X11 and Wayland dev
  packages to compile GLFW).
- At run time, `xvfb` and Mesa's GL (`libgl1-mesa-dri libglx-mesa0`). With
  `DISPLAY` already set (a real X server, an existing Xvfb, `xvfb-run`) it uses
  that instead of starting its own.

The NVIDIA container libraries (`NVIDIA_DRIVER_CAPABILITIES=all`) include
NVIDIA's GLX, but that only serves an NVIDIA X server. Against Xvfb, GLVND
picks Mesa, which is what is wanted: the interface is cheap to draw on the
CPU, and the GPU stays with training.

## How it works

`src/app/gui/RemoteDisplay.{h,cpp}`, `src/app/gui/RemoteInput.{h,cpp}`,
`src/app/gui/remote_gui.html` (embedded at build time).

**Frames.** After each frame is drawn, and only while a page is watching, the
back buffer is read and hashed; an unchanged frame is not sent, and none is
read more often than `--remote-fps`. A changed one is handed to an encoder
thread (stb's JPEG writer), so the frame loop never waits on encoding. The
newest JPEG goes to `/remote/stream`, an MJPEG stream
(`multipart/x-mixed-replace`) the page shows in an `<img>`: one trip across the
tunnel per frame instead of a request and a reply. An idle GUI sends nothing.

Two things about that stream are not obvious:

- Chrome shows a part only when the next part begins. A GUI that goes idle
  after a change would leave the page one frame behind, so the newest frame is
  sent a second time when nothing follows it within 150 ms.
- On Xvfb, the frame drawn right after a resize can land in the old-sized
  buffer and reads back shifted. That frame is skipped; the next is right.

`HttpServer` serves everything else one request at a time on one thread
(`src/app/webviewer/HttpServer.h`). A stream route (`route_stream`) is the
exception: each gets a thread of its own and keeps its connection, so input
requests are never stuck behind it. At most four streams run at once; a fifth
gets a 503.

**Input.** The page batches its events and sends them in order, one request in
flight, as a JSON array on `/remote/input?e=`. Mouse moves between two sends
collapse into the last one. The GUI thread applies them through ImGui's own
input queue (`io.AddMousePosEvent`, `AddKeyEvent`, ...), which spreads a press
and a release that arrive together over two frames, as a real click would be.
Each event wakes the frame loop (`glfwPostEmptyEvent`), which otherwise sleeps
up to a fifteenth of a second between idle frames.

Keys are sent by physical position (`KeyboardEvent.code`), so shortcuts follow
the key whatever the keyboard layout; typed characters are sent separately as
text. On a Mac, Cmd is sent as Ctrl, since Ctrl is what the GUI on the Linux
box listens for.

| event | fields |
|---|---|
| `m` | mouse at `x`,`y` (frame pixels) |
| `b` | button `b` (ImGui: 0 left, 1 right, 2 middle), `d` down |
| `w` | wheel `x`,`y`, in steps, positive up/left |
| `k` | key `code` (`KeyboardEvent.code`), `d` down, modifiers `c` `s` `a` `m` |
| `t` | text `v`, UTF-8 |
| `clip` | the browser's clipboard `v`, for the Ctrl+V that follows |
| `size` | the window's size `w`,`h`, 320x240 to 7680x4320 (clamped to the Xvfb screen, 5120x2880) |
| `f` | page focus `d`; losing it releases every held key |

**Clipboard.** ImGui's clipboard functions are replaced: what the GUI copies
is kept and announced through `/remote/status` (`clip` counts copies), and the
page fetches `/remote/clipboard` and writes it to the browser's clipboard.
Pasting goes the other way: the browser's `paste` event sends its text, then a
Ctrl+V.

**Links.** `gui::open_url` hands an http(s) link to the page instead of
starting a browser on the GPU box; the page shows it in a banner. Links from
before a page connected are not shown to it.

**File pickers.** The desktop's picker (`NativeDialog`) is off in a remote
session, so the GUI's own browser is used, which lists the GPU box's files.

| route | |
|---|---|
| `/` | the page |
| `/remote/stream` | MJPEG, boundary `spirulaframe` |
| `/remote/input?e=[...]` | the events above |
| `/remote/status?events=N` | `frame` (count of frames encoded), `w`/`h`, `cursor` (a CSS cursor name), `clip`, `drop` (where a drag onto the page goes), `events` newer than `N` (`url`, `viewer`, `download`, `upload`, `play`) and `last` |
| `/remote/clipboard` | what the GUI last copied, as text |

Every route wants `token=`, or the `ss_token_<port>` cookie the page sets.
The file routes (`/viewer/`, `/remote/raw/`, `/remote/download/`,
`/remote/upload`) are in [file-browser.md](file-browser.md).

## Measured

On loopback, from a click in Chromium to the changed frame being encoded: about
65 ms. A 1280x800 frame of the interface is 40-60 KB at quality 80, so a busy
30 frames a second is 1.5 MB/s and an idle GUI is nothing; a viewport showing a
splat is busier and larger. `src/app/gui/tests/remote_gui_test.cpp` covers the
input parsing and the stream routes over a socket.

## Not done

- Files dropped onto the page are uploaded only while the Files window is
  open ([file-browser.md](file-browser.md)); dropping one opens nothing.
- No IME composition: text that needs one cannot be typed.
- The page draws one frame pixel per CSS pixel, which is slightly soft on a
  high-DPI screen.
