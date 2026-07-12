# SyncBreaker soak testing — protocol and history

**Read this first if you are a testing agent picking up SyncBreaker
work.** Read `docs/syncbreaker.md` for the bug-class background,
then `docs/repro_r7_2026-07-12/handoff_syncbreaker5.md` for the
current-active-bug handoff.

---

## What a soak test does

Long-duration randomized coverage of the observer/agent join path.
Each round:

1. Kills any leftovers on the target port.
2. Picks a map + random race per slot.
3. Starts the openbw server with those races and a `--sync-log`.
4. Launches N agents + N observers **in a random shuffled order** with
   a fixed stagger between launches (default 20 s). N is the map's
   player count.
5. Plays a fixed wall-clock window (default 300 s at `--game-speed 10`,
   i.e. ~30k sim frames per round).
6. Kills all processes.
7. Runs an inline Python analyzer that diffs each observer's
   `INVENTORY` rows against the server's, tagging the round PASS /
   PASS_COSMETIC / FAIL_REAL / NO_DATA.
8. Appends one row to `results.tsv`. Keeps failure logs on disk;
   deletes PASS logs to save space.
9. Loops until the wall-clock budget expires.

Pass classification:
- **PASS**: every observer's INVENTORY (per-slot unit counts + in-progress
  counts) matches the server exactly across all shared frames.
- **PASS_COSMETIC**: match on unit counts, but observer's minerals or
  gas totals differ. Non-fatal — see `feedback_sync_log_off_by_one`
  memory or the analyzer comments.
- **FAIL_REAL**: an observer's completed or in-progress unit counts
  diverge from the server's on at least one shared frame. This is
  the signal that a SyncBreaker bug is active.
- **NO_DATA**: no shared frames between server + at least one
  observer (usually a launch failure).

---

## Two soak scripts

### `scripts/soak_syncbreaker.sh` — 2-player fixed map

Simpler variant. Always uses `(2)Bottleneck.scm`. Two slots, one
agent per slot, two observers (alice + bob viewing slots 0 and 1).
Randomizes race per slot and shuffles the 4-entity launch order.

```
scripts/soak_syncbreaker.sh                  # 1h default (12 rounds)
TOTAL_SECS=7200 scripts/soak_syncbreaker.sh  # 2h
PLAY_SECS=180  scripts/soak_syncbreaker.sh   # 3-min rounds
OUT=/tmp/soak-A scripts/soak_syncbreaker.sh  # custom output dir
```

Output goes to `/tmp/soak_syncbreaker/` by default:
- `results.tsv` — one row per round.
- `round-<N>_<verdict>_.../` — kept on FAIL_REAL for inspection.

### `scripts/soak_syncbreaker_multi.sh` — 2/4/6/8 player maps

Broader coverage. Each round picks a random map from
`original_resources/` matching one of {2, 4, 6, 8} player counts.
Launches N agents + N observers with a random shuffled order,
20 s stagger, 5 min play. Each observer uses the player key for
its own slot so it renders that slot's fog of war.

```
scripts/soak_syncbreaker_multi.sh                     # 1h default
TOTAL_SECS=7200 scripts/soak_syncbreaker_multi.sh     # 2h
PLAYERS_FILTER="2,4" scripts/soak_syncbreaker_multi.sh   # 2/4-player maps only
PLAYERS_FILTER="6" scripts/soak_syncbreaker_multi.sh      # 6-player only (SB#5 focus)
```

Output at `/tmp/soak_syncbreaker_multi/` by default.

**Scales up fast.** 8-player = 8 agents + 8 observers = 17 processes.
At `--game-speed 10` this is CPU-heavy. Watch Activity Monitor and
drop to `--game-speed 24` if the fleet can't keep up.

### Environment variables

Both scripts honor:
- `TOTAL_SECS` — wall-clock budget (default 3600)
- `PLAY_SECS` — game-play window per round after last launch (default 300)
- `STAGGER` — seconds between launches (default 20)
- `GAME_SPEED` — server `--game-speed` value (default 10)
- `PORT` — server WebSocket port (default 6114)
- `OUT` — output directory
- `MAP` (single-map script only) — override map path

Add `nohup ... > /tmp/soak.log 2>&1 &` when running for hours so the
soak keeps running if you close the terminal.

---

## Reading the results.tsv

Multi-map soak columns:
```
round  start_ts  map  players  races  launch_order
       verdict  worst_obs_real  total_shared  details  server_frames  notes
```

`details` is `observer1:shared/min_only/real; observer2:...` — you
can grep for `FAIL_REAL` and eyeball which observer(s) diverged and
by how many frames.

Single-map (2-player) columns:
```
round  start_ts  race0  race1  launch_order  verdict
       alice_shared  alice_min_only  alice_real
       bob_shared    bob_min_only    bob_real
       server_frames  notes
```

Prior published run: `docs/soak_2026-07-11_results.tsv` — 10 / 10
PASS on `(2)Bottleneck.scm` after the SyncBreaker #3 fix (commit
66f3305). Nice smoke-test baseline. Any regression that puts even
one FAIL_REAL into a 2-player soak is a new bug — start there.

---

## Recommended run schedule for a fresh tester

The **fastest** way to reproduce known SB#5 issues:

```
# 1. Confirm baseline: 2-player should stay green.
scripts/soak_syncbreaker.sh  # 1h, expect 12/12 PASS
```

Then:

```
# 2. Trigger SB#5 on 6-player maps.
PLAYERS_FILTER="6" scripts/soak_syncbreaker_multi.sh  # 1h, expect FAIL_REALs
```

Then, for a targeted deterministic repro (no randomness in launch
order or race selection):

```
scripts/repro_round7.sh  # ~9 min, config locked to reproduce
```

The `repro_round7` script is the input the SB#5 handoff was built
from. If a fix seems to work on `repro_round7`, re-run the multi
soak with `PLAYERS_FILTER="6"` for at least 2 hours to confirm the
random-launch-order variant is also green.

---

## What "FAIL_REAL" looks like

Typical FAIL row from run 3 of the SB#5 repro:
```
alice   matched= 318  min_only=  0  real=248  FAIL_REAL
bob     matched= 344  min_only=  0  real=248  FAIL_REAL
carol   matched= 266  min_only=  0  real=248  FAIL_REAL
dave    matched= 226  min_only=  0  real=226  FAIL_REAL
eve     matched= 212  min_only=  0  real=212  FAIL_REAL
frank   matched= 306  min_only=  0  real=248  FAIL_REAL
```

Interpretation: `matched` = observer INVENTORY frames that had a
server counterpart (with ±1 frame slack). `real` = frames where the
observer disagreed with server on completed/in-progress unit counts.
Whenever `real > 0` the game silently desynced.

To dive from soak-level "FAIL_REAL" to frame-level pinpointing, use
the per-frame diagnostics in `sync.h` — see the handoff document.

---

## Analysis toolbox

- `scripts/analyze_lcg_divergence.py` — earliest INVENTORY-frame
  where lcg_rand differs (theory-1 vs theory-2 branch).
- `scripts/analyze_lcg_tick.py` — earliest LCG_TICK frame where lcg
  differs (finer granularity, per 30 frames).
- `scripts/analyze_first_divergence.py` — earliest current_frame
  where TICK-carried lcg differs, plus 4-frame TICK context.
- `scripts/analyze_state_divergence.py` — earliest current_frame
  where per-owner state hash or unit count differs. Reports WHICH
  slot's units first drifted. **Use this on SB#5 sync-logs.**
- `scripts/tick_at_frame.py <lo> <hi> <sync1> [<sync2> ...]` — pull
  TICK rows in a `current_frame` window across N files.

---

## Historical soak notes

- **2026-07-11 (docs/soak_2026-07-11_results.tsv)**: 10 / 10 PASS on
  `(2)Bottleneck.scm`, after commit 66f3305 fixed SyncBreaker #3.
  Confirmed the fix on the 2-player case.
- **Same day, multi-map soak (task #113)**: exposed SB#5 (this doc's
  successor case) at 6-player maps. Round 7 of that soak was
  frozen as the `repro_round7.sh` config.
- **2026-07-12 (handoff)**: three focused reprod using
  `repro_round7.sh` with progressive instrumentation. Nailed SB#5
  divergence to slot-5 state at cf=14200 in a quiet sim tick. Full
  detail in `docs/repro_r7_2026-07-12/handoff_syncbreaker5.md`.

---

## Related documents

- `docs/syncbreaker.md` — SyncBreaker bug-class background + prior fixes.
- `docs/repro_r7_2026-07-12/handoff_syncbreaker5.md` — current open bug.
- `docs/repro_r7_2026-07-12/summary.md` — morning snapshot of same.
- `docs/soak_2026-07-11_results.tsv` — baseline 10/10 pass log.

## Related open tasks

- #114 SyncBreaker #5 (currently open)
- #115 Command queue slot-serial drain
- #116 Map loader unknown trigger action 12
- #117 SyncBreaker #5 late-joiner catchup bug
