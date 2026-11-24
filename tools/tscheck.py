#!/usr/bin/env python3
"""Check MPEG-TS recordings for integrity.

Usage: tscheck.py [--settle SECONDS] [--duration SECONDS] FILE.ts...

Skips the first --settle seconds (the demod needs ~1-1.5 s after lock,
during which packets carry TEI), then reports sync errors, packets with
the transport error indicator (TEI) set, continuity-counter errors and the
number of distinct PIDs. CC errors are listed per PID, so you can tell
broadcaster quirks (one data PID always discontinuous) from dropped
packets (errors spread over every PID, video first).

Exit status is 1 if any file has sync or TEI errors after settling.
"""
import argparse
import collections
import sys

PKT = 188


def check(path, settle, duration):
    with open(path, "rb") as f:
        data = f.read()
    n = len(data) // PKT
    if n == 0:
        print(f"{path}: EMPTY")
        return False
    skip = int(n * settle / duration) if duration else 0
    sync = tei = 0
    cc = {}
    cc_err = collections.Counter()
    pids = collections.Counter()
    for k in range(skip, n):
        p = data[k * PKT:(k + 1) * PKT]
        if p[0] != 0x47:
            sync += 1
            continue
        if p[1] & 0x80:
            tei += 1
            continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        pids[pid] += 1
        if pid == 0x1FFF or not (p[3] & 0x10):  # null, or no payload
            continue
        c = p[3] & 0x0F
        disc = (p[3] & 0x20) and p[4] > 0 and (p[5] & 0x80)
        if pid in cc and c != cc[pid] and c != (cc[pid] + 1) & 0x0F and not disc:
            cc_err[pid] += 1
        cc[pid] = c
    mbit = len(data) * 8 / duration / 1e6 if duration else 0
    worst = ", ".join(f"0x{p:x}:{e}/{pids[p]}" for p, e in cc_err.most_common(3))
    ok = sync == 0 and tei == 0
    print(f"{path}: {'OK ' if ok else 'BAD'} {mbit:5.2f} Mbit/s, "
          f"{n - skip} pkts checked, sync_err={sync}, tei={tei}, "
          f"cc_err={sum(cc_err.values())}{' (' + worst + ')' if worst else ''}, "
          f"pids={len(pids)}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--settle", type=float, default=2.0)
    ap.add_argument("--duration", type=float, default=20.0,
                    help="recording length, to convert --settle to packets")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    results = [check(f, a.settle, a.duration) for f in a.files]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
