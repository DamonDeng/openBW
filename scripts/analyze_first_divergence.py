#!/usr/bin/env python3
# analyze_first_divergence.py -- given server + observer sync-log with
# per-frame LCG_TICK + TICK trace, find the FIRST sim frame where lcg
# disagrees between server and observer, and dump the surrounding TICK
# trace from both sides.

import sys

def load_lcg_per_frame(path):
    """Returns {current_frame -> lcg_hex}.

    LCG_TICK lines carry sync_frame in parts[1] (from sync_log_line) but
    that's not what we want -- sync_frame is per-side and doesn't align
    across peers. Instead we cross-reference the TICK line at the SAME
    sync_frame to pull current_frame, then key by current_frame.
    """
    ticks = load_tick_trace(path)  # sync_frame -> (cf, lcg, vcs)
    # LCG_TICK lcg matches TICK lcg by construction (same tick), so we
    # can just re-project the tick trace by current_frame.
    out = {}
    for sync_f, (cf, lcg, _) in ticks.items():
        out[cf] = lcg
    return out

def load_tick_trace(path):
    """Returns {sync_frame -> (current_frame, lcg, vcs_str)}"""
    out = {}
    for ln in open(path):
        if "\tTICK\t" not in ln: continue
        parts = ln.rstrip().split("\t")
        try:
            sync_frame = int(parts[1])  # position after S/O tag
            d = {}
            for p in parts:
                if "=" in p:
                    k, v = p.split("=", 1)
                    d[k] = v
            out[sync_frame] = (
                int(d.get("current_frame", "-1")),
                d.get("lcg", "?"),
                d.get("vcs", "?"))
        except: pass
    return out

if len(sys.argv) < 3:
    print("usage: analyze_first_divergence.py <server.sync> <observer.sync>")
    sys.exit(1)

srv_lcg = load_lcg_per_frame(sys.argv[1])
obs_lcg = load_lcg_per_frame(sys.argv[2])

# Find first current_frame where both have LCG_TICK and they differ
common = sorted(set(srv_lcg.keys()) & set(obs_lcg.keys()))
if not common:
    print("no shared LCG_TICK frames")
    sys.exit(2)

first_diff = None
last_match = None
for f in common:
    if srv_lcg[f] == obs_lcg[f]:
        last_match = f
    else:
        first_diff = f
        break

if first_diff is None:
    print(f"lcg matched across all {len(common)} shared frames.")
    sys.exit(0)

print(f"first current_frame divergence: {first_diff}")
if last_match is not None:
    print(f"  last match: current_frame={last_match} lcg={srv_lcg[last_match]}")
else:
    print(f"  (no earlier matching frame -- diverged from first shared frame)")
print(f"  first diff: current_frame={first_diff} srv={srv_lcg[first_diff]} obs={obs_lcg[first_diff]}")
print()

# Load TICK traces
srv_ticks = load_tick_trace(sys.argv[1])
obs_ticks = load_tick_trace(sys.argv[2])

# Find sync_frame entries whose current_frame is in the divergence window
def ticks_near(traces, cf_lo, cf_hi):
    return sorted((sync_f, cf, lcg, vcs)
                  for sync_f, (cf, lcg, vcs) in traces.items()
                  if cf_lo <= cf <= cf_hi)

window_lo = last_match if last_match is not None else max(0, first_diff - 2)
srv_window = ticks_near(srv_ticks, window_lo, first_diff + 2)
obs_window = ticks_near(obs_ticks, window_lo, first_diff + 2)

print("=== SERVER ticks around divergence ===")
print(f"{'sync_f':>7s}  {'cf':>5s}  lcg         vcs")
for sync_f, cf, lcg, vcs in srv_window[:10]:
    print(f"{sync_f:7d}  {cf:5d}  {lcg:12s}  {vcs}")
print()
print("=== OBSERVER ticks around divergence ===")
print(f"{'sync_f':>7s}  {'cf':>5s}  lcg         vcs")
for sync_f, cf, lcg, vcs in obs_window[:10]:
    print(f"{sync_f:7d}  {cf:5d}  {lcg:12s}  {vcs}")
