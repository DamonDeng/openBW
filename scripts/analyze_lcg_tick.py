#!/usr/bin/env python3
# analyze_lcg_tick.py -- given a server sync-log and one observer sync-log,
# find the FIRST current_frame where the LCG_TICK lcg values disagree.

import sys

def load_lcg(path):
    out = {}
    for ln in open(path):
        if "LCG_TICK" not in ln: continue
        parts = ln.rstrip().split("\t")
        try:
            frame = int(parts[1])
            lcg = None
            for p in parts:
                if p.startswith("lcg="):
                    lcg = p.split("=")[1]
            if lcg is None: continue
            if frame not in out:
                out[frame] = lcg
        except (ValueError, IndexError):
            pass
    return out

if len(sys.argv) < 3:
    print("usage: analyze_lcg_tick.py <server.sync> <observer.sync>")
    sys.exit(1)

srv = load_lcg(sys.argv[1])
obs = load_lcg(sys.argv[2])

common_frames = sorted(set(srv.keys()) & set(obs.keys()))
if not common_frames:
    print("no shared LCG_TICK frames")
    sys.exit(2)

print(f"first shared: {common_frames[0]}  last shared: {common_frames[-1]}  count: {len(common_frames)}")

first_diff = None
last_match = None
for f in common_frames:
    if srv[f] == obs[f]:
        last_match = f
    else:
        first_diff = f
        break

if first_diff is None:
    print("LCG matched across shared window. Observer fully synced.")
else:
    print(f"LCG diverges:")
    print(f"  last match:   frame {last_match}  lcg={srv[last_match]}")
    print(f"  first diff:   frame {first_diff}  srv={srv[first_diff]}  obs={obs[first_diff]}")
    if last_match is not None:
        print(f"  window: ({last_match}, {first_diff}]  size: {first_diff - last_match} frames")
    print()
    idx = common_frames.index(first_diff)
    print("next 5 shared frames:")
    for f in common_frames[idx:idx+5]:
        tag = "MATCH" if srv[f] == obs[f] else "DIFF"
        print(f"  frame {f:6d}  srv={srv[f]}  obs={obs[f]}  {tag}")
