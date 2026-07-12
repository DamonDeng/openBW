#!/usr/bin/env python3
# analyze_state_divergence.py -- given server + observer sync-logs with
# per-tick TICK trace enriched with per-owner state hashes and counts,
# find the FIRST current_frame where any owner slot's hash or count
# diverges, and pin which OWNER first drifted.
#
# Log format:
#   TICK sync_frame=X current_frame=Y lcg=... n=n0,n1,...,nN
#        h0=... h1=... ... h7=... hN=...

import sys

def load_state_trace(path):
    """Returns {cf -> {lcg, n[0..8], h[0..8]}} using h8=hN, n8=neutral count."""
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
            entry = {"lcg": d.get("lcg", "?")}
            ns = d.get("n", "").split(",")
            if len(ns) != 9: continue
            entry["n"] = ns
            entry["h"] = [d.get(f"h{i}", "?") for i in range(8)] + [d.get("hN", "?")]
            out[cf] = entry
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

# Find first divergence per column (lcg, n0..n8, h0..h8).
first = {}
def add_field(key, cf, sv, ov):
    if sv != ov and key not in first:
        first[key] = (cf, sv, ov)

last_all_match = None
for cf in common:
    s = srv[cf]; o = obs[cf]
    if s == o:
        last_all_match = cf
    add_field("lcg", cf, s["lcg"], o["lcg"])
    for i in range(9):
        add_field(f"n{i}", cf, s["n"][i], o["n"][i])
        add_field(f"h{i}", cf, s["h"][i], o["h"][i])
    if len(first) == 1 + 9 + 9:
        break

print(f"shared TICK frames: {len(common)}  range [{common[0]},{common[-1]}]")
print(f"last frame where all fields matched: {last_all_match}")
print()

# Group by earliest cf
groups = {}
for k, (cf, s, o) in first.items():
    groups.setdefault(cf, []).append((k, s, o))
if not groups:
    print("no divergence in shared window")
    sys.exit(0)

# Sort by cf ascending
earliest_cf = min(groups.keys())
print(f"EARLIEST divergence: cf={earliest_cf}")
print(f"  fields that split at this frame:")
for k, s, o in sorted(groups[earliest_cf]):
    print(f"    {k:4s}: server={s} observer={o}")
print()

# Windowed diff of counts + hashes for +-3 frames
idx = common.index(earliest_cf)
lo = max(0, idx - 3); hi = min(len(common), idx + 4)
print("nearby ticks (S=server, O=observer):")
print(f"{'cf':>6s}  {'lcg_S':>8s} {'lcg_O':>8s}  "
      f"{'nS':>3s}={'..':>17s} {'nO':>3s}={'..':>17s}")
for i in range(lo, hi):
    cf = common[i]
    s = srv[cf]; o = obs[cf]
    marker = " <-- DIVERGE" if cf == earliest_cf else ""
    ns = ",".join(s["n"][:8]) + f"|N{s['n'][8]}"
    no = ",".join(o["n"][:8]) + f"|N{o['n'][8]}"
    print(f"{cf:6d}  {s['lcg']:>8s} {o['lcg']:>8s}  {ns:>21s} {no:>21s}{marker}")
print()

# For hashes, show the per-owner delta at earliest_cf
s = srv[earliest_cf]; o = obs[earliest_cf]
print(f"per-owner hash at cf={earliest_cf} (arrow marks disagreement):")
print(f"{'owner':>7s}  {'nS':>4s} {'nO':>4s}  {'hS':>10s}  {'hO':>10s}")
for i in range(9):
    tag = "  <--" if (s["n"][i] != o["n"][i] or s["h"][i] != o["h"][i]) else ""
    name = "neutral" if i == 8 else str(i)
    print(f"{name:>7s}  {s['n'][i]:>4s} {o['n'][i]:>4s}  {s['h'][i]:>10s}  {o['h'][i]:>10s}{tag}")

# One-frame-earlier peek: was there a hash split at cf-1?
if idx > 0:
    prev_cf = common[idx-1]
    ps = srv[prev_cf]; po = obs[prev_cf]
    prev_diffs = []
    for i in range(9):
        if ps["n"][i] != po["n"][i]: prev_diffs.append(f"n{i}")
        if ps["h"][i] != po["h"][i]: prev_diffs.append(f"h{i}")
    print()
    if prev_diffs:
        print(f"  cf={prev_cf}: pre-existing splits: {prev_diffs}")
    else:
        print(f"  cf={prev_cf}: ALL matched, divergence emerged this tick.")
