#!/usr/bin/env python3
"""MCP stdio server over the two headless web surfaces.

`spirula sfm auto --monitor PORT` (or `spirula sfm monitor WS`) serves a
reconstruction; `spirula train` serves the training viewer. This exposes both
as MCP tools, so a coding agent can ask how a run on a remote GPU is going --
through the same SSH tunnel a browser would use -- without a page open.

Configured by environment, each URL with its token:
  SS_MONITOR_URL  (default http://127.0.0.1:7008)   SS_MONITOR_TOKEN
  SS_VIEWER_URL   (default http://127.0.0.1:7007)   SS_VIEWER_TOKEN
Routes and formats: docs/notes/remote-monitoring.md. Standard library only.
"""

import base64
import json
import math
import os
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mcp_stdio  # noqa: E402

# sfm::Stage, in order (src/sfm/core/Events.h).
STAGES = ["extract", "match", "map", "merge", "orient", "finish", "load",
          "select", "seed", "refine", "focal", "progressive"]


def endpoint(which):
    if which == "sfm":
        return (os.environ.get("SS_MONITOR_URL", "http://127.0.0.1:7008").rstrip("/"),
                os.environ.get("SS_MONITOR_TOKEN", ""))
    return (os.environ.get("SS_VIEWER_URL", "http://127.0.0.1:7007").rstrip("/"),
            os.environ.get("SS_VIEWER_TOKEN", ""))


def fetch(which, path, params=None, timeout=15.0):
    """The body as bytes; None on a 404 (nothing written yet)."""
    base, token = endpoint(which)
    q = dict(params or {})
    if token:
        q["token"] = token
    url = base + path + ("?" + urllib.parse.urlencode(q) if q else "")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        if e.code == 403:
            raise SystemExit("403 from %s: set %s to the token the run printed"
                             % (base, "SS_MONITOR_TOKEN" if which == "sfm" else "SS_VIEWER_TOKEN"))
        raise SystemExit("%s%s: HTTP %d %s" % (base, path, e.code, e.read()[:200]))
    except urllib.error.URLError as e:
        raise SystemExit("cannot reach %s (%s); is the run up and the port tunnelled?"
                         % (base, e.reason))


def get_json(which, path, params=None):
    b = fetch(which, path, params)
    return None if b is None else json.loads(b)


def text(obj):
    s = obj if isinstance(obj, str) else json.dumps(obj, ensure_ascii=False, indent=2)
    return [{"type": "text", "text": s}], False


# ===========================================================================
# SfM
# ===========================================================================

def sfm_status(a):
    s = get_json("sfm", "/status")
    s["stage_name"] = STAGES[s["stage"]] if 0 <= s["stage"] < len(STAGES) else str(s["stage"])
    s.pop("stamps", None)
    return text(s)


def sfm_log(a):
    since = a.get("since")
    if since is None:
        s = get_json("sfm", "/status")
        if not s.get("live"):
            return text("This monitor has no log: only a run started with "
                        "`spirula sfm auto --monitor` serves one.")
        since = max(0, s["log"] - int(a.get("tail", 100)))
    r = get_json("sfm", "/log", {"since": int(since)})
    grep = (a.get("grep") or "").lower()
    lines = [l for l in r["lines"] if not grep or grep in l["text"].lower()]
    if a.get("level"):
        lines = [l for l in lines if l["level"] == a["level"]]
    body = "\n".join(l["text"] for l in lines)
    return text("%s\n-- next since=%d" % (body, r["next"]))


def sfm_images(a):
    r = get_json("sfm", "/images")
    if r is None:
        return text("No image statistics yet: the mapper writes them once a model exists.")
    imgs = r["images"]
    placed = [i for i in imgs if i["placed"]]
    unplaced = [i for i in imgs if not i["placed"]]
    out = {"images": len(imgs), "placed": len(placed), "unplaced": len(unplaced),
           "error_scale_px": [r["green_px"], r["red_px"]]}
    n = int(a.get("worst", 20))
    if a.get("unplaced"):
        out["unplaced_images"] = [{"name": i["name"], "keypoints": i["keypoints"]}
                                  for i in unplaced[:n]]
    else:
        by = a.get("by", "error")
        key = (lambda i: i["mean"]) if by == "error" else (lambda i: i["points"])
        out["worst_by_" + by] = [
            {k: i[k] for k in ("name", "points", "keypoints", "mean", "max")}
            for i in sorted(placed, key=key, reverse=(by == "error"))[:n]]
    return text(out)


def sfm_models(a):
    r = get_json("sfm", "/models")
    return text(r or {"sizes": []})


def sfm_progressive(a):
    b = fetch("sfm", "/progressive")
    return text(b.decode("utf-8", "replace") if b else
                "No progressive.txt: the run is not progressive, or has not finished mapping.")


def sfm_match_summary(a):
    b = fetch("sfm", "/pairs")
    if b is None:
        return text("No match map yet.")
    if b[:4] != b"SSPM":
        raise SystemExit("unexpected /pairs payload")
    n_images, bins, peak, staged = struct.unpack_from("<4I", b, 4)
    cells = bins * bins
    counts = struct.unpack_from("<%dI" % cells, b, 20)
    row = [sum(counts[r * bins:(r + 1) * bins]) for r in range(bins)]
    linked = [sum(1 for c in counts[r * bins:(r + 1) * bins] if c) for r in range(bins)]
    out = {"images": n_images, "bins": bins, "peak_inliers": peak,
           "cells_with_inliers": sum(1 for c in counts if c)}
    if staged:
        planned = struct.unpack_from("<%dI" % cells, b, 20 + cells * 4)
        verified = struct.unpack_from("<%dI" % cells, b, 20 + cells * 8)
        out["pairs_planned"] = sum(planned) // 2
        out["pairs_verified"] = sum(verified) // 2
    per_bin = max(1, -(-n_images // bins))
    weakest = sorted(range(bins), key=lambda r: (row[r], linked[r]))[:int(a.get("weakest", 10))]
    out["weakest_" + ("images" if per_bin == 1 else "image_blocks")] = [
        {"images": [r * n_images // bins, (r + 1) * n_images // bins - 1],
         "inliers": row[r], "linked": linked[r]} for r in weakest]
    return text(out)


# ===========================================================================
# Training
# ===========================================================================

def look_at(eye, target):
    """Row-major 3x4 camera-to-world, OpenGL axes (looks down -Z, +Y up),
    +Z up in the world: the convention viewer.html sends."""
    f = [t - e for t, e in zip(target, eye)]
    fl = math.sqrt(sum(v * v for v in f)) or 1.0
    f = [v / fl for v in f]
    up = [0.0, 0.0, 1.0]
    if abs(f[2]) > 0.999:
        up = [0.0, 1.0, 0.0]
    r = [f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]]
    rl = math.sqrt(sum(v * v for v in r)) or 1.0
    r = [v / rl for v in r]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    return [r[0], u[0], -f[0], eye[0],
            r[1], u[1], -f[1], eye[1],
            r[2], u[2], -f[2], eye[2]]


def scene_centre():
    """The viewer's default orbit centre, the cameras' median position."""
    s = get_json("train", "/scene") or {}
    for m in s.get("center_modes", []):
        if m.get("name") == "camera-median":
            return m["center"]
    return [0.0, 0.0, 0.0]


def train_render(a):
    w, h = int(a.get("width", 640)), int(a.get("height", 480))
    focal = 0.5 * w / math.tan(math.radians(float(a.get("fov", 60))) / 2)
    if a.get("c2w"):
        c2w = [float(v) for v in str(a["c2w"]).split(",")]
    else:
        target = [float(v) for v in a.get("target") or scene_centre()]
        yaw, pitch = math.radians(float(a.get("yaw", -90))), math.radians(float(a.get("pitch", 18)))
        d = float(a.get("distance", 1.0))
        eye = [target[0] + d * math.cos(pitch) * math.cos(yaw),
               target[1] + d * math.cos(pitch) * math.sin(yaw),
               target[2] + d * math.sin(pitch)]
        c2w = look_at(eye, target)
    params = {"buffer_key": a.get("buffer", "rgb"),
              "c2w": ",".join("%.6g" % v for v in c2w),
              "fx": focal, "fy": focal, "cx": w / 2, "cy": h / 2,
              "width": w, "height": h, "jpeg_quality": int(a.get("quality", 80)),
              "show_training_cameras": "1" if a.get("cameras") else "0"}
    img = fetch("train", "/render", params, timeout=30.0)
    if img is None:
        raise SystemExit("this viewer has no /render")
    return [{"type": "image", "data": base64.b64encode(img).decode("ascii"),
             "mimeType": "image/jpeg"}], False


TOOLS = [
    {"name": "sfm_status",
     "description": "Where a reconstruction is: stage, done/total, images placed, "
                    "points, models, mean reprojection error, finished/partial.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "sfm_log",
     "description": "The run's log lines (only for `sfm auto --monitor`). Default: "
                    "the last 100; pass `since` (the `next` a previous call gave) "
                    "to read on from there.",
     "inputSchema": {"type": "object", "properties": {
         "tail": {"type": "integer"},
         "since": {"type": "integer"},
         "level": {"type": "string", "enum": ["info", "note", "warning", "error", "diag"]},
         "grep": {"type": "string", "description": "Case-insensitive substring."}}}},
    {"name": "sfm_images",
     "description": "Per-image fit: the worst placed images by mean reprojection "
                    "error or by fewest 3D points, or the images not placed.",
     "inputSchema": {"type": "object", "properties": {
         "worst": {"type": "integer", "description": "How many (default 20)."},
         "by": {"type": "string", "enum": ["error", "points"]},
         "unplaced": {"type": "boolean"}}}},
    {"name": "sfm_models",
     "description": "Images placed in each model the run has written.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "sfm_progressive",
     "description": "progressive.txt: each progressive alignment attempt and what it placed.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "sfm_match_summary",
     "description": "The match map in numbers: pairs planned/verified, the peak, and "
                    "the images (or image blocks) with the fewest inliers to anything.",
     "inputSchema": {"type": "object", "properties": {
         "weakest": {"type": "integer", "description": "How many (default 10)."}}}},
    {"name": "train_progress",
     "description": "The training viewer's /progress: step, ETA, latency, paused.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "train_render",
     "description": "Render the model being trained, as an image. Orbit around "
                    "`target` (normalized frame, +Z up; default the cameras' median, as the "
                    "viewer starts) by yaw/pitch/distance, or "
                    "pass a row-major 3x4 OpenGL c2w as 12 comma-separated numbers.",
     "inputSchema": {"type": "object", "properties": {
         "yaw": {"type": "number", "description": "Degrees (default -90: from -Y)."},
         "pitch": {"type": "number", "description": "Degrees (default 18)."},
         "distance": {"type": "number", "description": "Default 1."},
         "target": {"type": "array", "items": {"type": "number"}},
         "c2w": {"type": "string"},
         "fov": {"type": "number", "description": "Horizontal, degrees (default 60)."},
         "width": {"type": "integer"}, "height": {"type": "integer"},
         "buffer": {"type": "string", "description": "A key from /buffers; default rgb."},
         "cameras": {"type": "boolean", "description": "Draw the training cameras."},
         "quality": {"type": "integer"}}}},
    {"name": "train_pause_toggle",
     "description": "Pause or resume training. Returns the new state.",
     "inputSchema": {"type": "object", "properties": {}}},
]


def run_tool(name, a):
    if name == "sfm_status":
        return sfm_status(a)
    if name == "sfm_log":
        return sfm_log(a)
    if name == "sfm_images":
        return sfm_images(a)
    if name == "sfm_models":
        return sfm_models(a)
    if name == "sfm_progressive":
        return sfm_progressive(a)
    if name == "sfm_match_summary":
        return sfm_match_summary(a)
    if name == "train_progress":
        return text(get_json("train", "/progress"))
    if name == "train_render":
        return train_render(a)
    if name == "train_pause_toggle":
        return text(get_json("train", "/pause-toggle"))
    return [{"type": "text", "text": "unknown tool: " + str(name)}], True


def main():
    mcp_stdio.serve("spirula-monitor", TOOLS, run_tool)


if __name__ == "__main__":
    main()
