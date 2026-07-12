#!/usr/bin/env python3
# tick_at_frame.py -- given N sync-log files and a target current_frame
# window, dump the TICK line from each side at each current_frame in the
# range. Useful for eyeballing the exact moment the vc-frame vectors
# diverge across peers.
#
# Usage:
#   tick_at_frame.py <lo> <hi> <sync.log> [<sync.log>...]

import sys, os

def load_tick_trace(path):
    """Returns list of (current_frame, sync_frame, lcg, vcs)."""
    out = []
    for ln in open(path):
        if "\tTICK\t" not in ln: continue
        parts = ln.rstrip().split("\t")
        d = {}
        try:
            for p in parts:
                if "=" in p:
                    k, v = p.split("=", 1)
                    d[k] = v
            out.append((int(d.get("current_frame", "-1")),
                        int(d.get("sync_frame", "-1")),
                        d.get("lcg", "?"),
                        d.get("vcs", "?")))
        except: pass
    return out

if len(sys.argv) < 4:
    print("usage: tick_at_frame.py <lo> <hi> <sync.log> [<sync.log>...]")
    sys.exit(1)

lo = int(sys.argv[1]); hi = int(sys.argv[2])
files = sys.argv[3:]

print(f"{'file':40s}  {'cf':>5s}  {'sync_f':>7s}  lcg         vcs")
for f in files:
    label = os.path.basename(f)
    for cf, sf, lcg, vcs in load_tick_trace(f):
        if lo <= cf <= hi:
            print(f"{label:40s}  {cf:5d}  {sf:7d}  {lcg:12s}  {vcs}")
    print()
