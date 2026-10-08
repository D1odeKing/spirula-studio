#!/usr/bin/env python3
"""The GUI licence prompt, driven by tools/guictl.py against the real binary.

    python3 tools/license_checks/license_prompt_gate.py --exe build/spirula

A gated download opens the licence dialog instead of failing. Every family's
Accept stays off until the "I have read and accept" tick, cancelling downloads
nothing and says so, and accepting starts the download by itself; a batch asks
up front, for the built-in families and a registered one (RoMa) alike. Private
config and cache, so the licences and the models are this run's own. A download
it lets start is seen running, then cancelled by quitting the window.
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("guictl", HERE.parent / "guictl.py")
guictl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guictl)

failures = 0


def check(ok, what):
    global failures
    print(("ok    " if ok else "FAIL  ") + what)
    failures += 0 if ok else 1


def ctl(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        guictl.main(list(argv))
    return out.getvalue()


def state():
    return guictl.call("/ui/state")


def until(pred, what, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.3)
    check(False, "timed out waiting for " + what)
    return False


def reveal(item_id):
    """Scroll the form until `item_id` is on screen."""
    for _ in range(12):
        rows = [i["rect"] for i in guictl.call("/ui/tree", {"named": "1", "q": item_id})["items"]
                if i["id"] == item_id]
        if rows and 100 < rows[0][1] < 560:
            return
        ctl("scroll", "--at", "400,400", "--dy", "-12")


def pick(combo_id, text):
    reveal(combo_id)
    ctl("click", combo_id)
    hits = [i for i in guictl.call("/ui/tree", {"named": "0", "q": text})["items"]
            if i["id"] != combo_id and text in i["label"]]
    if not hits:
        check(False, "no entry %r in %s" % (text, combo_id))
        return False
    x0, y0, x1, y1 = hits[0]["rect"]
    ctl("click", "--at", "%d,%d" % ((x0 + x1) / 2, (y0 + y1) / 2))
    return True


def png(path, w=64, h=48):
    raw = b"".join(b"\0" + b"".join(bytes([(x * 4) % 256, (y * 5) % 256, 128]) for x in range(w))
                   for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def alive(pid):
    return subprocess.run(["ps", "-p", str(pid), "-o", "comm="], capture_output=True,
                          text=True).stdout.strip() != ""


def quit_gui(pid, exe):
    """Quit through the menu; signal only a process that is verifiably ours."""
    with contextlib.suppress(Exception, SystemExit):
        ctl("click", "menu_file")
        ctl("click", "menu_quit")
    end = time.time() + 15
    while time.time() < end and alive(pid):
        time.sleep(0.5)
    if alive(pid):
        args = subprocess.run(["ps", "-p", str(pid), "-o", "args="], capture_output=True,
                              text=True).stdout.strip()
        if args.split(" ")[0] == exe:
            os.kill(pid, 15)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--port", type=int, default=7814)
    ap.add_argument("--shots", help="directory for screenshots of each dialog")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    work = pathlib.Path(tempfile.mkdtemp(prefix="license_gui_gate_"))
    cfg, cache = work / "config", work / "cache"
    os.environ.update(XDG_CONFIG_HOME=str(cfg), XDG_CACHE_HOME=str(cache),
                      SS_GUI_AUTOMATION_PORT=str(a.port))
    conf = cfg / "spirula-studio" / "gui.conf"
    models = cache / "spirula-studio" / "models"
    imgs = work / "imgs"
    imgs.mkdir()
    for i in range(3):
        png(imgs / ("f%d.png" % i))
    accepted = lambda fam: conf.exists() and ("accepted_license=%s\n" % fam) in conf.read_text()
    downloaded = lambda: sorted(p.name for p in models.glob("*")) if models.exists() else []
    shot = lambda name: a.shots and ctl("shot", str(pathlib.Path(a.shots) / name))
    if a.shots:
        pathlib.Path(a.shots).mkdir(parents=True, exist_ok=True)

    pid = json.loads(ctl("launch", "--exe", exe, "--offscreen", "--port", str(a.port),
                         "--log", str(work / "gui.log")))["pid"]
    try:
        ctl("click", "home_new_dataset")
        ctl("drop", str(imgs))
        until(lambda: state()["screen"] == "new_dataset", "the New Dataset screen")
        ctl("click", "mask_enable")
        until(lambda: guictl.call("/ui/tree", {"named": "1", "q": "mask_get_model"})["items"],
              "the Get the model button")

        # ---- every family: the dialog opens, Accept needs the tick, Cancel is clean ----
        # (combo text, the licence family it asks for)
        cases = [("SAM 3 (most", "sam3"), ("SAM 2.1 Base+", "sam2"), ("BiRefNet (most", "birefnet")]
        for text, fam in cases:
            if not pick("mask_model", text):
                continue
            ctl("click", "mask_get_model")
            s = state()
            check(s["license_prompt"] == fam and s["model_download"] == "idle",
                  "%s: the dialog opens instead of the download failing (prompt=%r)" %
                  (text, s["license_prompt"]))
            shot("%s_unticked.png" % fam)
            ctl("click", "license_download")
            s = state()
            check(s["license_prompt"] == fam and not accepted(fam) and s["model_download"] == "idle",
                  "%s: Download does nothing without the tick" % fam)
            ctl("click", "license_accept_tick")
            check(state()["license_tick"], "%s: the tick takes" % fam)
            shot("%s_ticked.png" % fam)
            ctl("click", "cancel")
            s = state()
            check(s["license_prompt"] == "" and not accepted(fam) and not downloaded() and
                  s["model_download"] == "idle" and s["license_notice"],
                  "%s: Cancel records nothing, downloads nothing, and says so" % fam)

        # ---- accepting continues by itself ---------------------------------
        pick("mask_model", "SAM 2.1 Base+")
        ctl("click", "mask_get_model")
        until(lambda: state()["license_prompt"] == "sam2", "the SAM 2.1 dialog")
        ctl("click", "license_accept_tick")
        ctl("click", "license_download")
        until(lambda: state()["license_prompt"] != "sam2", "the dialog to close")
        check(accepted("sam2"), "SAM 2.1: accepted and recorded in gui.conf")
        s = state()
        check(s["license_prompt"] == "gdino",
              "... then the text detector's own licence is asked (prompt=%r)" % s["license_prompt"])
        check(not s["license_tick"], "... with a fresh, unticked box")
        ctl("click", "license_download")
        check(state()["license_prompt"] == "gdino" and not accepted("gdino"),
              "Grounding DINO: Download does nothing without the tick either")
        ctl("click", "license_accept_tick")
        ctl("click", "license_download")
        until(lambda: state()["model_download"] == "running", "the download to start by itself", 20)
        s = state()
        check(accepted("gdino") and s["model_download"] == "running" and s["license_prompt"] == "",
              "accepting the last licence starts the download with no further click")

        # ---- a batch asks once, up front, for everything it needs ------------
        # Fresh GUI: the first half left sam2 and gdino accepted and a download running.
        quit_gui(pid, exe)
        until(lambda: not alive(pid), "the first window to close", 20)
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(cache, ignore_errors=True)
        pid = json.loads(ctl("launch", "--exe", exe, "--offscreen", "--port", str(a.port),
                             "--log", str(work / "gui2.log")))["pid"]
        until(lambda: state()["app_ready"], "the second window")
        ctl("click", "home_batch")
        until(lambda: state()["screen"] == "batch", "the Batch screen")
        ctl("drop", str(imgs))
        until(lambda: guictl.call("/ui/tree", {"named": "1", "q": "batch_start"})["items"],
              "the Start button")
        # The default preset masks nothing; the 360 camera preset removes people, so the
        # row needs the masking checkpoint and its text detector.
        ctl("click", "--at", "300,362")
        entry = [i for i in guictl.call("/ui/tree", {"named": "0", "q": "360-camera"})["items"]]
        check(bool(entry), "the 360 camera preset is offered for the row")
        x0, y0, x1, y1 = entry[0]["rect"]
        ctl("click", "--at", "%d,%d" % ((x0 + x1) / 2, (y0 + y1) / 2))
        ctl("click", "batch_start")
        s = state()
        check(s["license_prompt"] == "sam2" and not s["batch_fetching"] and
              s["model_download"] == "idle",
              "Start asks for the first licence the batch needs, before anything runs "
              "(prompt=%r)" % s["license_prompt"])
        shot("batch_first.png")
        ctl("click", "cancel")
        s = state()
        check(s["license_prompt"] == "" and not s["batch_fetching"] and s["license_notice"] and
              not accepted("sam2") and not downloaded() and s["model_download"] == "idle",
              "declining stops the batch: nothing recorded, nothing downloaded, and it says so")
        ctl("click", "batch_start")
        ctl("click", "license_accept_tick")
        ctl("click", "license_accept")
        s = state()
        check(accepted("sam2") and s["license_prompt"] == "gdino" and not s["batch_fetching"] and
              s["model_download"] == "idle",
              "the next licence is asked at once, still before any download (prompt=%r)" %
              s["license_prompt"])
        ctl("click", "license_accept_tick")
        ctl("click", "license_accept")
        until(lambda: state()["batch_fetching"], "the batch to start fetching", 20)
        s = state()
        check(accepted("gdino") and s["license_prompt"] == "" and
              until(lambda: state()["model_download"] == "running", "the fetch", 20),
              "after the last answer the batch fetches what it needs by itself")

        # ---- a registered family: a dense row asks for RoMa up front too ------
        # No built-in preset turns dense on, so the row reads one written here.
        quit_gui(pid, exe)
        until(lambda: not alive(pid), "the second window to close", 20)
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(cache, ignore_errors=True)
        preset = cfg / "spirula-studio" / "presets" / "dataset" / "dense-on.json"
        preset.parent.mkdir(parents=True)
        preset.write_text(json.dumps({"kind": "dataset", "name": "dense-on",
                                      "settings": {"dense_enable": True}}))
        (cfg / "spirula-studio" / "batch.json").write_text(json.dumps({"rows": [{
            "sources": [str(imgs)], "dataset": str(work / "ds"),
            "dataset_preset": {"path": str(preset), "name": "dense-on"},
            "stages": [True, False, False, False], "enabled": True}]}))
        roma = lambda: any(models.glob("romav2.0.1.pt*")) if models.exists() else False
        pid = json.loads(ctl("launch", "--exe", exe, "--offscreen", "--port", str(a.port),
                             "--log", str(work / "gui3.log")))["pid"]
        until(lambda: state()["app_ready"], "the third window")
        ctl("click", "home_batch")
        until(lambda: state()["screen"] == "batch", "the Batch screen")
        ctl("click", "batch_start")
        s = state()
        check(s["license_prompt"] == "roma" and not s["batch_fetching"] and not roma(),
              "RoMa: Start asks for its licence before anything runs (prompt=%r)" %
              s["license_prompt"])
        shot("roma_batch.png")
        ctl("click", "cancel")
        s = state()
        check(s["license_prompt"] == "" and not s["batch_fetching"] and s["license_notice"] and
              not accepted("roma") and not roma(),
              "RoMa: declining stops the batch, records nothing, downloads nothing")
        ctl("click", "batch_start")
        ctl("click", "license_accept_tick")
        ctl("click", "license_accept")
        until(lambda: state()["batch_fetching"], "the batch to start fetching", 20)
        check(accepted("roma") and until(roma, "the RoMa download", 30),
              "RoMa: after accepting, the batch fetches the checkpoint by itself")
    finally:
        quit_gui(pid, exe)
        shutil.rmtree(work, ignore_errors=True)
    print("\nFAILED: %d" % failures if failures else "\nall passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
