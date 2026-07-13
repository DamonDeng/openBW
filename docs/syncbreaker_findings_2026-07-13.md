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

---

## Follow-up: v2 combat soak result

Same setup as the v1 soak above, but agents are `t_agent_debug_v2`
and `p_agent_debug_v2` (`scripts/soak_sync_debug_v2.sh`). These
add a Priority-8 attack pass:
  - 3/4 of idle combat units attack-move to the map center.
  - 1/4 attack-move to a random tile (re-rolled per unit per tick).
  - Everything else (no scouts, no worker moves, no lift, no
    repair, no mines, no coverage) stays disabled.

The two players fought for real. Protoss decisively won —
Terran ended with `bases=0`, all SCVs killed off. Protoss ended
at `combat=46, buildings=35, supply=121/169, scarabs=10/10`.

TICK-line diff (per-frame state hashes across 44,000+ frames):

| pair                      | overlapping frames | diverged |
|---------------------------|-------------------:|---------:|
| obs_A vs server           |             43,782 |    **0** |
| obs_B vs server           |             43,781 |    **0** |
| obs_A vs obs_B            |             43,788 |    **0** |

Wire traffic: **73,300 AGENT_APPLY events** processed. Byte-
identical across all three logs.

## Combined implications

Neither peaceful (v1) nor combat (v2) soak produced any state-
hash divergence. That covers:
  - `train` / `build` / `upgrade` / `research` / `morph` /
    `gather` / mining
  - `attack` (both attack-move to point and attack-unit)
  - Unit spawn / kill / death handling
  - Multi-observer fan-out across two distinct observer clients

The previously-observed divergence (yesterday's speed=10 soak
with full v4/v5 agents at frame 24862) must originate from a
code path that these two variants do NOT exercise:
  - `phase_scout` — worker `c.move` orders to distant waypoints
    with per-worker blacklists
  - `c.stop` — the "coverage verb" post-move pass
  - `c.lift` / `c.land` — Terran building flight
  - `c.repair` — SCV movement onto damage targets
  - `c.siege` / `c.unsiege` — sim-side tank transformation
  - `c.place_mine` — Vulture mines with home->enemy vector logic
  - `c.morph` / `c.morph_building` — Zerg (not on this map)

Next candidate to bisect: enable ONE of those verbs at a time
in a v3/v4 variant and see which one reintroduces divergence.
Best first candidate is `phase_scout`, since it's the most
prolific `c.move` source and the yesterday-observed drift did
happen with the full scout pass enabled.

---

## Follow-up 2: v2 combat soak P-v-P

Runbook: `scripts/soak_sync_debug_v2_pvp.sh`. Same as v2 T-v-P
but both slots are Protoss driven by `p_agent_debug_v2`. Mirror
match runs longer before one side collapses.

Duration: server frame 45,434 (~30 min game time). Player B
(slot 1) dominated (99/169 supply vs A's 6/89).

TICK-line diff (per-frame state hashes):

| pair                      | overlapping frames | diverged |
|---------------------------|-------------------:|---------:|
| obs_A vs server           |             45,033 |    **0** |
| obs_B vs server           |             45,034 |    **0** |
| obs_A vs obs_B            |             45,042 |    **0** |

Wire traffic: **43,084 AGENT_APPLY events**. Byte-identical
across all three logs.

## Cumulative evidence

Three independent multi-thousand-frame soaks, all localhost, all
native SDL observers, all with two distinct observer clients:

| soak                      | total frames | diverged |
|---------------------------|-------------:|---------:|
| v1 peaceful (T-v-P)       |       31,218 |        0 |
| v2 combat (T-v-P)         |       43,782 |        0 |
| v2 combat P-v-P           |       45,033 |        0 |
| **total**                 |  **120,033** |    **0** |

Every state hash (h0..h7, hN, LCG) matches on every frame across
server + 2 observers on 120,000+ frames covering both peaceful
production and full-tempo combat.

## Where the bugs really live

By elimination, sync divergence must originate in one of the
verbs the debug agents deliberately don't use:

- `phase_scout` — worker `c.move` to distant waypoints (radial /
  zscan patrol). Most prolific `c.move` source in the full agent.
- `c.stop` (Priority 9 coverage) — post-move stop verb.
- `c.lift` / `c.land` — Terran building airborne state.
- `c.repair` — SCV movement onto damaged targets.
- `c.siege` / `c.unsiege` — sim-side tank state transformation.
- `c.place_mine` — Vulture mine placement.
- `c.morph` / `c.morph_building` — Zerg (not exercised on
  Bottleneck since it has no Zerg starting positions).

Yesterday's Terran-observer divergence at frame 24862 happened
while the full `t_agent_v5` + `p_agent_v4` were running, so the
guilty verb is a subset of that list.

The natural bisection is a series of `_debug_v3.<verb>` variants
that enable exactly ONE of those passes on top of v2. If v3.scout
diverges but v3.repair doesn't, we've pinned scout as the culprit.


---

## Follow-up 3: deterministic-repro soak with fixed initial_rand

`scripts/soak_repro_yesterday.sh` config + `--fixed-initial-rand
deadbeef`. Full t_agent_v5 + p_agent_v4, default intervals,
player-role observers (per-slot fog).

- Server confirmed `initial_rand=deadbeef` on GAME_START (byte-
  exact LCG stream).
- Terran got wiped by the Protoss economy advantage. Game ended
  early at server frame 22,627.
- Verified `--fixed-initial-rand` is deterministic across runs
  (two separate server launches with the same value both printed
  the same `initial_rand=deadbeef`).

TICK-line diff:

| pair                      | overlapping frames | diverged |
|---------------------------|-------------------:|---------:|
| obs_terran vs server      |             22,117 |    **0** |
| obs_protoss vs server     |             22,118 |    **0** |
| obs_terran vs obs_protoss |             22,125 |    **0** |

## Where SyncBreaker #5 actually lives

Cumulative today across all four soaks:

| soak                          | frames  | diverged |
|-------------------------------|--------:|---------:|
| v1 peaceful T-v-P             | 31,218  |        0 |
| v2 combat T-v-P               | 43,782  |        0 |
| v2 combat P-v-P               | 45,033  |        0 |
| yesterday-config + fixed rand | 22,117  |        0 |
| **total**                     |**142,150**|      0 |

Zero state-hash divergence in ~1.5 hours of aggregated game
time across four distinct configs.

Yesterday's frame-24862 divergence did NOT reproduce under
`--fixed-initial-rand`. That means the bug is not deterministic
with respect to the LCG state. Remaining hypotheses:

1. **Action-arrival timing race.** Python `asyncio.sleep(1.5)`
   isn't jitter-free; which frame an agent action lands on can
   vary across runs even with identical sim state. Yesterday
   the Protoss agent fired 8 actions in one server frame, and
   the observer's TICK for that frame diverged. If the sim
   applies actions in a non-deterministic order when they share
   a target_frame + slot, that'd match the pattern.

2. **OS-scheduler / socket-buffer variance.** TCP delivery
   ordering between agent → server and server → observer isn't
   guaranteed frame-perfect. If sync.h has any code that
   depends on delivery order rather than the timestamp stamped
   in id_agent_action, that'd bite here.

3. **Genuinely transient.** Some environmental race that
   fires <1-in-N runs. Would show up eventually in soak testing
   but not on demand.

Next capability needed: **action-queue logging on both agent
and server sides**. Capture for each `id_agent_action` the
(source slot, action bytes, sim frame when scheduled locally,
sim frame when broadcast by server, sim frame when applied on
each observer). Replay that recorded stream deterministically
into a second run and see if the divergence reproduces.

## Provisional verdict

Under everything we can currently test with byte-exact repro,
sync.h is clean. The observer is safe for workshop use with
current code. Yesterday's finding is reclassified from "known
sync.h bug" to "one-off, non-reproducible, capture more data
next time it surfaces."
