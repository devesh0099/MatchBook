#!/usr/bin/env python3
"""A DEV STAND-IN for ioi/isolate — never for the event.

Speaks exactly the slice of isolate's CLI that platform/worker/src/sandbox.rs
uses (--init / --run / --cleanup, the meta file, --inherit-fds), so the
platform's pipeline can be exercised end to end on a machine without isolate
installed. It provides NO isolation whatsoever: the child runs as the current
user with the worker's own privileges. The sandbox properties themselves are
verified against real isolate (SANDBOX.md); this exists so pipeline work does
not require a provisioned box.

    ISOLATE_BIN=/path/to/fake-isolate.py worker --role agent

Meta statuses mirror isolate's: TO (wall-time exceeded), SG (died on a
signal), RE (non-zero exit). Box directories live under
$FAKE_ISOLATE_ROOT (default /tmp/fake-isolate)/<box-id>/box.
"""

import os
import shutil
import signal
import subprocess
import sys

ROOT = os.environ.get("FAKE_ISOLATE_ROOT", "/tmp/fake-isolate")


def main() -> int:
    args = sys.argv[1:]
    box_id = "0"
    meta = None
    wall_time = None
    envs = []
    inherit_fds = False
    mode = None
    argv = []

    i = 0
    while i < len(args):
        a = args[i]
        if a == "--box-id":
            i += 1
            box_id = args[i]
        elif a == "--meta":
            i += 1
            meta = args[i]
        elif a == "--wall-time":
            i += 1
            wall_time = float(args[i])
        elif a in ("--time", "--cg-mem"):
            i += 1  # accepted, unenforced
        elif a.startswith("--processes"):
            pass  # accepted, unenforced
        elif a.startswith("--env="):
            envs.append(a[len("--env="):])
        elif a.startswith("--dir="):
            pass
        elif a == "--inherit-fds":
            inherit_fds = True
        elif a in ("--init", "--cleanup", "--run"):
            mode = a
            if a == "--run":
                rest = args[i + 1:]
                argv = rest[1:] if rest and rest[0] == "--" else rest
                break
        elif a == "--cg":
            pass
        i += 1

    base = os.path.join(ROOT, box_id)
    box = os.path.join(base, "box")

    if mode == "--init":
        os.makedirs(box, exist_ok=True)
        print(base)
        return 0
    if mode == "--cleanup":
        shutil.rmtree(base, ignore_errors=True)
        return 0
    if mode != "--run" or not argv:
        print("fake-isolate: nothing to do", file=sys.stderr)
        return 2

    env = {"HOME": box}
    for kv in envs:
        k, _, v = kv.partition("=")
        env[k] = v

    pass_fds = []
    if inherit_fds:
        try:
            os.fstat(9)
            pass_fds = [9]
        except OSError:
            pass

    def write_meta(fields: dict) -> None:
        if meta:
            with open(meta, "w") as f:
                for k, v in fields.items():
                    f.write(f"{k}:{v}\n")

    try:
        proc = subprocess.Popen(argv, cwd=box, env=env, pass_fds=pass_fds)
        try:
            rc = proc.wait(timeout=wall_time)
        except subprocess.TimeoutExpired:
            proc.send_signal(signal.SIGKILL)
            proc.wait()
            write_meta({"status": "TO", "killed": 1})
            return 1
    except FileNotFoundError as e:
        print(f"fake-isolate: {e}", file=sys.stderr)
        return 2

    if rc < 0:
        write_meta({"status": "SG", "exitsig": -rc, "killed": 1})
        return 1
    if rc != 0:
        write_meta({"status": "RE", "exitcode": rc})
        return 1
    write_meta({"exitcode": 0})
    return 0


if __name__ == "__main__":
    sys.exit(main())
