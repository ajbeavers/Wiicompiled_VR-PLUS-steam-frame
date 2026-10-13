#!/usr/bin/env python3
"""Samples the game's read I/O every 50 ms: KF_IOSAMPLE_EXE=EXE iosample.py OUT.tsv (lane E, frame/perf/run.sh).

Lines: CLOCK_MONOTONIC seconds, rchar (bytes read by read()-type calls, cached or not), read_bytes
(bytes that came from the block device), and the game data files newly open since the last sample
(paths under a DATA/files directory). Waits for a process of EXE that starts after the sampler."""
import os
import sys
import time


def find_game(exe, before):
    for pid in os.listdir("/proc"):
        if pid.isdigit() and pid not in before:
            try:
                if os.readlink("/proc/%s/exe" % pid) == exe:
                    return pid
            except OSError:
                pass
    return None


def main(out, exe):
    exe = os.path.realpath(exe)
    before = set(p for p in os.listdir("/proc") if p.isdigit())
    pid = None
    while pid is None:
        pid = find_game(exe, before)
        if pid is None:
            time.sleep(0.2)
    seen = set()
    with open(out, "w") as f:
        f.write("# pid %s\n# mono\trchar\tread_bytes\tnew_files\n" % pid)
        while True:
            try:
                io = dict(l.split(": ") for l in open("/proc/%s/io" % pid).read().split("\n") if l)
                files = set()
                for fd in os.listdir("/proc/%s/fd" % pid):
                    try:
                        p = os.readlink("/proc/%s/fd/%s" % (pid, fd))
                    except OSError:
                        continue
                    if "/files/" in p:
                        files.add(p.split("/files/", 1)[1])
            except (OSError, ValueError):
                return
            new = sorted(files - seen)
            seen = files
            f.write("%.3f\t%s\t%s\t%s\n" % (time.monotonic(), io.get("rchar", "0"), io.get("read_bytes", "0"), ",".join(new)))
            f.flush()
            time.sleep(0.05)


if __name__ == "__main__":
    main(sys.argv[1], os.environ["KF_IOSAMPLE_EXE"])
