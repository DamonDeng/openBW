#!/usr/bin/env python3
# analyze_state_divergence.py -- given server + observer sync-logs with
# per-tick TICK trace enriched with (nu, ht, od, tp) state hashes, find
# the FIRST current_frame where any state-hash field diverges, and
# report which field split. This nails down whether the divergence is
# in unit-count, hp/position, orders, or type/ownership.
#
# Fields:
#   nu = live unit count
#   ht = FNV-1a hash of {shield+hp, exact_pos.x, exact_pos.y}
#   od = FNV-1a hash of {order_type.id, order_state, main_order_timer}
#   tp = FNV-1a hash of {unit_type.id, owner}
#
# A field that splits ONE tick BEFORE the lcg splits identifies the
# state that fed the differing lcg call.

import sys

def load_state_trace(path):
    """Returns {current_frame -> (lcg, nu, ht, od, tp)}"""
    out = {}
    for ln in open(path):
        if "\tTICK\t" not in ln: continue
        parts = ln.rstrip().split("\t")
        d = {}
        try:
            for p in parts:
                if "=" in p:
                    k, v = p.split("=", 1)
                    d[k] = v
            cf = int(d.get("current_frame", "-1"))
            out[cf] = (
                d.get("lcg", "?"),
                d.get("nu", "?"),
                d.get("ht", "?"),
                d.get("od", "?"),
                d.get("tp", "?"),
            )
        except: pass
    return out

if len(sys.argv) < 3:
    print("usage: analyze_state_divergence.py <server.sync> <observer.sync>")
    sys.exit(1)

srv = load_state_trace(sys.argv[1])
obs = load_state_trace(sys.argv[2])
common = sorted(set(srv.keys()) & set(obs.keys()))
if not common:
    print("no shared TICK frames")
    sys.exit(2)

# Walk forward, find first divergence per field.
fields = ["lcg", "nu", "ht", "od", "tp"]
first_diff_of = {f: None for f in fields}
last_all_match = None
for cf in common:
    s = srv[cf]; o = obs[cf]
    for idx, name in enumerate(fields):
        if s[idx] != o[idx] and first_diff_of[name] is None:
            first_diff_of[name] = cf
    if s == o:
        last_all_match = cf
    elif all(first_diff_of[f] is not None for f in fields):
        break

print(f"shared TICK frames: {len(common)}  range [{common[0]},{common[-1]}]")
print(f"last frame where all fields matched: {last_all_match}")
print()
print(f"{'field':6s}  {'first_diff':>10s}  {'server':>10s}  {'observer':>10s}")
for f in fields:
    cf = first_diff_of[f]
    if cf is None:
        print(f"{f:6s}  {'never':>10s}")
    else:
        idx = fields.index(f)
        print(f"{f:6s}  {cf:10d}  {srv[cf][idx]:>10s}  {obs[cf][idx]:>10s}")
print()

# Which field diverged FIRST? That's the culprit bucket.
earliest_cf = None
earliest_field = None
for f in fields:
    if first_diff_of[f] is None: continue
    if earliest_cf is None or first_diff_of[f] < earliest_cf:
        earliest_cf = first_diff_of[f]
        earliest_field = f
if earliest_field:
    print(f"EARLIEST divergence: {earliest_field} at cf={earliest_cf}")
    if earliest_field != "lcg":
        # If state diverged before lcg, we have a smoking gun.
        lcg_diff = first_diff_of.get("lcg")
        if lcg_diff is not None:
            gap = lcg_diff - earliest_cf
            print(f"  state field '{earliest_field}' diverged {gap} frames BEFORE lcg.")
    print()
    # dump 3 frames leading up to and 3 frames after
    idx = common.index(earliest_cf)
    lo = max(0, idx - 3); hi = min(len(common), idx + 3)
    print(f"{'cf':>7s}  {'lcg_S':>8s}  {'lcg_O':>8s}  "
          f"{'nu_S':>4s} {'nu_O':>4s}  {'ht_S':>8s} {'ht_O':>8s}  "
          f"{'od_S':>8s} {'od_O':>8s}  {'tp_S':>8s} {'tp_O':>8s}")
    for i in range(lo, hi):
        cf = common[i]
        s = srv[cf]; o = obs[cf]
        marker = " <-- DIVERGE" if cf == earliest_cf else ""
        print(f"{cf:7d}  {s[0]:>8s}  {o[0]:>8s}  "
              f"{s[1]:>4s} {o[1]:>4s}  {s[2]:>8s} {o[2]:>8s}  "
              f"{s[3]:>8s} {o[3]:>8s}  {s[4]:>8s} {o[4]:>8s}{marker}")
