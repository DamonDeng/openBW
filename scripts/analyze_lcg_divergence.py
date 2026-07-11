#!/usr/bin/env python3
# analyze_lcg_divergence.py -- given server + observer sync-logs, find the
# earliest frame where the observer's lcg_rand_state disagrees with the
# server's. Distinguishes:
#   * lcg diverges BEFORE INVENTORY diverges  -> nondeterministic sim
#     (some code path uses randomness that isn't captured by initial_rand
#     and lcg_rand alone).
#   * lcg stays synced AND INVENTORY diverges -> action-to-state mismatch
#     (identical rng, identical actions, different result -> hidden state).
#
# Usage:
#   scripts/analyze_lcg_divergence.py /tmp/repro_r7/{server,obs_bob}.sync

import sys, re

def load(path):
    """Returns dict[(frame, slot)] -> (lcg_hex, completed_str, in_progress_str)"""
    out = {}
    with open(path) as f:
        for ln in f:
            if "INVENTORY" not in ln: continue
            parts = ln.rstrip().split("\t")
            try:
                frame = int(parts[1])
                slot = int(parts[3].split("=")[1])
                lcg_field = [p for p in parts if p.startswith("lcg=")]
                lcg = lcg_field[0].split("=")[1] if lcg_field else "unknown"
                comp_field = [p for p in parts if p.startswith("completed=")]
                comp = comp_field[0] if comp_field else ""
                inp_field = [p for p in parts if p.startswith("in_progress=")]
                inp = inp_field[0] if inp_field else ""
                out[(frame, slot)] = (lcg, comp, inp)
            except (IndexError, ValueError):
                pass
    return out

def compare(server_path, obs_path):
    srv = load(server_path)
    obs = load(obs_path)
    common = sorted(set(srv.keys()) & set(obs.keys()))
    if not common:
        # Try +/-1 frame alignment
        aligned = []
        for k in sorted(obs.keys()):
            f, s = k
            for off in (0, 1, -1):
                k2 = (f+off, s)
                if k2 in srv:
                    aligned.append((k, k2))
                    break
        if not aligned:
            print("no shared inventory keys, even with +/-1 frame slack")
            return
        # Report first divergence
        print(f"aligned {len(aligned)} entries with +/-1 slack")
    else:
        aligned = [(k, k) for k in common]

    lcg_diverges_at = None
    completed_diverges_at = None
    n_lcg_match = 0
    n_completed_match = 0
    for (ok, sk) in aligned:
        so = srv[sk]; ob = obs[ok]
        if so[0] == ob[0]:
            n_lcg_match += 1
        elif lcg_diverges_at is None:
            lcg_diverges_at = (ok, sk, ob[0], so[0])
        if so[1] == ob[1] and so[2] == ob[2]:
            n_completed_match += 1
        elif completed_diverges_at is None:
            completed_diverges_at = (ok, sk, ob, so)

    print(f"shared entries: {len(aligned)}")
    print(f"  lcg matched:       {n_lcg_match}")
    print(f"  completed matched: {n_completed_match}")
    print()
    if lcg_diverges_at:
        ok, sk, obs_lcg, srv_lcg = lcg_diverges_at
        print(f"FIRST LCG DIVERGENCE: obs frame {ok} vs server frame {sk}")
        print(f"  observer lcg: {obs_lcg}")
        print(f"  server   lcg: {srv_lcg}")
    else:
        print("LCG never diverged across the whole run.")
    print()
    if completed_diverges_at:
        ok, sk, ob, so = completed_diverges_at
        print(f"FIRST COMPLETED/IN_PROGRESS DIVERGENCE: obs frame {ok} vs server frame {sk}")
        print(f"  observer: lcg={ob[0]} {ob[1]} {ob[2]}")
        print(f"  server:   lcg={so[0]} {so[1]} {so[2]}")
    else:
        print("Unit counts never diverged.")

    # Verdict
    print()
    if lcg_diverges_at and completed_diverges_at:
        ll_frame = lcg_diverges_at[0][0]
        cc_frame = completed_diverges_at[0][0]
        if ll_frame < cc_frame:
            print("VERDICT: LCG diverges BEFORE unit counts -> theory (1): nondeterministic sim.")
            print("         Something uses randomness that isn't captured/reproduced faithfully.")
        elif ll_frame == cc_frame:
            print("VERDICT: LCG and unit-counts diverge SIMULTANEOUSLY at frame " f"{ll_frame}.")
            print("         Same underlying bug touches both.")
        else:
            print("VERDICT: unit counts diverge BEFORE LCG -> theory (2): identical rng,")
            print("         different action-to-state mapping. Hidden state variable is desyncing.")
    elif completed_diverges_at and not lcg_diverges_at:
        print("VERDICT: LCG stays synced, but unit counts diverge. Theory (2): action-to-state")
        print("         mapping differs. Look for non-rng inputs to the sim (unit iteration")
        print("         order, container hashing, memory-order-dependent code).")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: analyze_lcg_divergence.py <server.sync> <observer.sync>")
        sys.exit(1)
    compare(sys.argv[1], sys.argv[2])
