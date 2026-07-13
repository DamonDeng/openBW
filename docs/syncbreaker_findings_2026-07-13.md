# SyncBreaker findings — 2026-07-13 soak

## Summary

**sync.h is clean under peaceful multi-observer load.** The
long-standing "observer drift" family of bugs (SyncBreaker #1-#6)
appears to be narrower than previously assumed: divergence
happens only when agents issue **combat/movement** verbs, not on
the general `train` / `build` / `upgrade` / `research` /
`morph` / mining code paths.

## Setup

- All localhost (Mac). No network, no ALB, no WASM.
- `openbw_server --game-speed 10` (100 FPS).
- One Terran player + one Protoss player, each driven by a
  purpose-built "peaceful" agent that spams production-side
  verbs but never attacks or moves units:
    - `python_agent/agents/t_agent_debug_v1.py`
    - `python_agent/agents/p_agent_debug_v1.py`
- **Two** native SDL observers, both using `role=observer,
  assigned_slot=-1` for full-map / no-fog vision. Each observer
  authenticated with a distinct key (`sk-w1`, `sk-w2`) so the
  server registers them as two separate clients (single-key
  registration collapses to one client, which is a separate
  server-side dedup behavior; see "Notes" below).
- Duration: ~5 min real time, ~21 min game time, server frame
  38,934. Observers ran to frame ~31,600 (user closed windows).

Runbook: `scripts/soak_sync_debug.sh` (edited to use two
observer keys).

## Result

TICK-line diff (per-frame state hashes: `h0`, `h1`, `hN`,
`lcg`, unit counters) across all three sync-logs:

| pair                      | overlapping frames | diverged |
|---------------------------|-------------------:|---------:|
| obs_A vs server           |             31,218 |    **0** |
| obs_B vs server           |             31,667 |    **0** |
| obs_A vs obs_B            |             31,216 |    **0** |

**Zero divergences anywhere.** All three logs are byte-identical
on every per-frame state hash across 31,000+ frames.

An "all rows" diff (TICK + LCG_TICK + AGENT_APPLY +
AGENT_SCHED_LOCAL + AGENT_SCHED_SEND + INVENTORY + GAME_START)
shows ~6,800 apparent AGENT_APPLY row differences, but those
are all pre-existing logging artifacts:
- The join key `frame|type|slot` isn't unique for AGENT_APPLY —
  multiple actions land at the same slot on the same frame, and
  the diff join collides across them.
- The specific field that differs is `vc_frame` (a per-client
  virtual-clock counter that is naturally different on the
  server side vs the observer side by design). Documented in
  `feedback_sync_log_off_by_one` memory.
- The `lcg`, `sim_frame`, and `target_frame` fields match.

So the "all rows" figure is diff-tool noise, not divergence.

## Contrast with previous findings

Yesterday's local speed=10 soak (2026-07-12) with the SAME
server/observer binaries but the FULL t_agent_v5 + p_agent_v4
(which do attacks, movement, siege, etc.):

- Terran observer diverged at frame 24862, right when the
  Protoss agent fired 8 near-simultaneous actions in one server
  frame.
- The observer's `h0` (Terran state) stayed synced but `h1`
  (Protoss state) drifted, and LCG then locked at a wrong
  value.

The delta between the two runs is: **combat/movement actions +
unit deaths/kills**. Everything else is unchanged. So whatever
sync.h issue exists, it's confined to the code paths that
handle `attack` / `move` / kill-target resolution — not to the
observer replay layer as a whole.

## Implications

- The classic hypothesis "observer state drifts silently over
  time" is **wrong** for peaceful gameplay. Simulation is
  deterministic and observer replay tracks it exactly.
- The real bug lives in the sim's action-application code path
  for attack/move/kill, not in the wire or the fan-out.
- Multi-observer fan-out through `sync_server_asio_ws` is
  correct — two independent clients get byte-identical replays.
- The `role=observer, assigned_slot=-1` "no fog" path also
  works correctly and is safe to use for spectator UIs.

## Next steps

The natural next soak is the same setup but with **combat
enabled** on the debug agents: keep the "no scout, no lift, no
mine" changes, but let combat units march to the map center and
fight. If a divergence reappears, it isolates the failing verb
to attack/move; the AGENT_APPLY log lines around the first
diverged frame will show which specific `id_agent_action`
byte-sequence hits the bug.

Two follow-up debug variants (planned, not yet implemented):

- `p_agent_debug_v2` and `t_agent_debug_v2` — same as v1 but
  with a new "combat drive" pass that:
    - Sends 3/4 of the idle combat units on `attack` orders to
      the map center at pixel (map_w/2, map_h/2).
    - Sends the remaining 1/4 on `attack` orders to a random
      point on the map, one point per unit, re-rolled per tick.
    - Keeps everything else (train, build, upgrade, research)
      firing at the same 0.1 s cadence.
- The v1 agents stay in tree as the "peaceful control" case for
  A/B comparisons.

Followed by a targeted soak: two observers, combat-enabled
debug agents, ~15-30 min game time. Diff sync-logs same way.

## Notes

- **Same-key observer dedup**: on the first attempt both
  observers authenticated with `sk-w` and the server reported
  `observers=1` even though two SDL processes were up. Only one
  of the two observer sync-logs grew — the other stalled at
  frame 300. Distinct keys resolved it. This is a real
  observation about auth behavior but not a sync bug.
- **Observer sync-log length differed slightly** (obs_A: 31,218
  TICK lines, obs_B: 31,667). Both stopped when the user closed
  the SDL window; obs_A shut down slightly earlier. Both agree
  with the server through their respective ranges.

## Data location

- `/tmp/simsc-logs/server_sync.log` — 12 MB, 99,049 lines
- `/tmp/simsc-logs/obs_A_sync.log` — 10 MB, 83,149 lines
- `/tmp/simsc-logs/obs_B_sync.log` — 10 MB, 84,063 lines
- Analysis intermediates: `/tmp/simsc-analysis/*.txt`

Retention: these are `/tmp` files. Kept as long as this Mac
doesn't reboot. If the finding needs to be independently
re-verified, re-run `scripts/soak_sync_debug.sh` — the setup is
deterministic given the same `seed` (default 42).
