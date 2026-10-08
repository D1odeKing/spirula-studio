#!/usr/bin/env python3
"""The CLI licence prompt, through the real binary on a real pseudo-terminal.

    python3 tools/license_checks/cli_prompt_gate.py --exe build/spirula

A SAM checkpoint named with --model needs its licence. On a terminal the whole
text is printed and the user types `yes`; anything else refuses and records
nothing. Each assertion reads gui.conf or the process's own exit status.
"""
import argparse
import os
import pathlib
import pty
import select
import shutil
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
failures = 0


def check(ok, what):
    global failures
    print(("ok    " if ok else "FAIL  ") + what)
    failures += 0 if ok else 1


def run_pty(exe, args, env, answers, cwd):
    """Run on a pty, sending each (wait_for, reply) in turn. -> (exit code, output)."""
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        os.execve(exe, [exe] + args, env)
    out = b""
    pending = list(answers)
    deadline = time.time() + 60
    status = None
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                break
            out += chunk
        if pending and pending[0][0].encode() in out:
            os.write(fd, (pending.pop(0)[1] + "\n").encode())
    _, status = os.waitpid(pid, 0)
    return (os.waitstatus_to_exitcode(status),
            out.decode("utf-8", "replace").replace("\r\n", "\n"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    work = pathlib.Path(tempfile.mkdtemp(prefix="license_cli_gate_"))
    conf = work / "config" / "spirula-studio" / "gui.conf"
    env = dict(os.environ, XDG_CONFIG_HOME=str(work / "config"),
               XDG_CACHE_HOME=str(work / "cache"))
    cwd = work / "cwd"
    cwd.mkdir()
    sam3 = (ROOT / "LICENSES" / "SAM3-License.txt").read_text()
    argv = ["sam", "segment", "--image", "x.png", "--text", "cat", "--model", "sam3-f16.ggml"]
    prompt = "Type 'yes'"
    accepted = lambda fam: conf.exists() and ("accepted_license=%s\n" % fam) in conf.read_text()

    try:
        # A line of the text from each end of the file: all of it is on the screen.
        first, last = sam3.splitlines()[0], [l for l in sam3.splitlines() if l.strip()][-1].strip()

        rc, out = run_pty(exe, argv, env, [(prompt, "no")], cwd)
        check(prompt in out and "read and accept the terms of SAM License (Meta)" in out,
              "a terminal is asked to confirm it has read and accepts the terms")
        check(first in out and last in out,
              "the licence is printed in full, first line and last (%r ... %r)" % (first, last[:30]))
        check(rc != 0 and not accepted("sam3"), "answering 'no' refuses and records nothing (rc=%d)" % rc)

        rc, out = run_pty(exe, argv, env, [(prompt, "y")], cwd)
        check(rc != 0 and not accepted("sam3"), "answering 'y' is not 'yes': refused, nothing recorded")

        rc, out = run_pty(exe, argv, env, [(prompt, "")], cwd)
        check(rc != 0 and not accepted("sam3"), "an empty answer refuses")

        rc, out = run_pty(exe, argv, env, [(prompt, "yes")], cwd)
        check(accepted("sam3"), "answering 'yes' records accepted_license=sam3")
        check("was not accepted" not in out, "... and the run goes on past the gate")

        rc, out = run_pty(exe, argv, env, [], cwd)
        check(prompt not in out, "once accepted it is not asked again")

        # A second family on the same terminal.
        rc, out = run_pty(exe, ["sam", "segment", "--image", "x.png", "--text", "cat", "--model",
                                "sam2.1_hiera_tiny_f16.ggml"], env, [(prompt, "yes")], cwd)
        check(accepted("sam2") and "Apache License" in out,
              "SAM 2.1 asks on its own, and prints the Apache-2.0 text")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\nFAILED: %d" % failures if failures else "\nall passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
