# SyncBreaker #5 handoff — repro + first-frame divergence pinned to slot 5

**Status: unresolved. Repro is stable.** This document hands the case
off to whoever picks it up next; it covers everything through today
(2026-07-12) so the next investigator does not need to re-derive it.

---

## TL;DR

SyncBreaker #5: with 6 players playing on `(6)New Gettysburg.scm` at
`--game-speed 10`, multi-observer runs desync silently. AGENT_APPLY
events are byte-identical on server and observer; the divergence
emerges DURING a quiet sim tick with no action applied.

Latest repro (run 3, per-owner state hashes on TICK) shows the
divergence is **NOT a race between observers** — all four
early-joined observers land on the SAME wrong slot-5 state hash on
the SAME sim frame. The trigger position varies run to run (races
in ingestion order?), but once triggered, every observer that saw
the same catchup replay walks the same wrong path.

Late-joiner observers (dave, eve) diverge instantly on their first
shared TICK frame — this is a different bug (broken catchup).

---

## Repro

```
scripts/repro_round7.sh
```

Configuration (locked in the script):
- Map: `original_resources/(6)New Gettysburg.scm`
- Slot races: `protoss, protoss, terran, terran, terran, terran`
- Server `--game-speed 10` (100 FPS)
- Launch order: `[O:1 A:0 O:0 O:5 A:2 A:5 O:2 A:4 A:1 O:3 O:4 A:3]`
- 20s stagger between launches
- 300s play window after the last launch
- Post-run analysis: `scripts/repro_round7.sh` runs `analyzer_multi.py`
  and prints per-observer PASS/FAIL

Sample output on a FAIL run (this is run 3, 2026-07-12):
```
alice   matched= 318  min_only=  0  real=248  FAIL_REAL
bob     matched= 344  min_only=  0  real=248  FAIL_REAL
carol   matched= 266  min_only=  0  real=248  FAIL_REAL
dave    matched= 226  min_only=  0  real=226  FAIL_REAL
eve     matched= 212  min_only=  0  real=212  FAIL_REAL
frank   matched= 306  min_only=  0  real=248  FAIL_REAL
```

Variability across runs: sometimes 2/6 fail, sometimes 6/6. Same
map, same script, same launch order.

---

## Current diagnostic instrumentation

Latest commit: `48fd64c sync.h: partitioned state hashes on per-tick TICK log`

The sync-log (enabled with `--sync-log <path>` on both server and
observers) now carries, per sim tick, on both sides:

- `LCG_TICK\tlcg=<hex>` (added dee09a2, every sim frame)
- `TICK\tsync_frame=X\tcurrent_frame=Y\tlcg=Z\tn=n0,n1,...,n7,nN\th0=…\th1=…\t...\th7=…\thN=…\tvcs=v0,v1,...,v7`
  - `n_i` = count of visible_units with owner=i (i==8 is neutral)
  - `h_i` = FNV-1a over each such unit's
    `{shield+hp raw, exact_pos.x, exact_pos.y, order_type.id,
      order_state, main_order_timer}`
  - `vcs` = per-slot `virtual_client_t::frame` counters

Sync-log also carries:
- `AGENT_APPLY\tslot=…\tsim_frame=…\ttarget_frame=…\tvc_frame=…\tlcg=…\tn_bytes=…\tbytes=…`
  emitted just BEFORE each scheduled action's `action_f` is called,
  identical format on server and observer (see `sync.h:352` — the
  `execute_scheduled_actions` guard).
- `INVENTORY` at 300-frame cadence (minerals, gas, per-slot unit
  counts + lcg_rand_state).
- `GAME_START\tinitial_rand=…` at server start / observer catchup
  handshake.

### Analyzers

- `scripts/analyze_first_divergence.py <server.sync> <observer.sync>`
  — first `current_frame` where lcg differs, plus 4-frame window
  of `TICK` context.
- `scripts/analyze_state_divergence.py <server.sync> <observer.sync>`
  — first frame where ANY per-owner hash or count differs. This is
  the tool that identifies WHICH SLOT'S UNITS diverged first.
- `scripts/analyze_lcg_tick.py` (older, per-30-frame LCG_TICK)
- `scripts/analyze_lcg_divergence.py` (INVENTORY-level lcg check)
- `scripts/tick_at_frame.py <lo> <hi> <sync1> [<sync2> …]` — dump
  TICK rows across N sync-logs in a `current_frame` window.

---

## Run-3 findings (run: `/tmp/repro_r7_owners/`, 2026-07-12 ~11:35)

Preserved excerpts under `docs/repro_r7_2026-07-12/runs/repro_r7_owners/`:

### Early joiners (alice, bob, carol, frank) — deterministic

All four diverge at **cf=14200**. At cf=14199, every field on both
sides matches identically. At cf=14200:

```
per-owner hash at cf=14200 (arrow marks disagreement):
  owner    nS   nO          hS          hO
      0    39   39    fa91d283    fa91d283
      1     5    5    d92e6745    d92e6745
      2    20   20    8c6d0a2b    8c6d0a2b
      3     5    5    949d42e7    949d42e7
      4     5    5    e70690e2    e70690e2
      5    11   11    5d3ef719    97a4b0cb  <-- slot-5 hash differs
      6     0    0    811c9dc5    811c9dc5
      7     0    0    811c9dc5    811c9dc5
neutral   173  173    c117ead0    c117ead0
```

**Only slot 5's `h5` splits. Count matches (11 units on both sides).**
So at least one of slot-5's 11 units has different (hp+position) OR
(order/state/timer) — not a new spawn, not a death, not an owner change.

**Same wrong observer hash across alice/bob/carol/frank**: all four
observers land on `h5 = 97a4b0cb`. This is deterministic across the
early-joined cohort.

lcg matches (`74a9f7a9`) at cf=14200 for both sides. lcg only diverges
later (167 sim frames later for alice in run 2), presumably when the
next lcg-consuming code path touches slot-5 unit state.

### Last slot-5 action before divergence

At sim_frame=14186 (13 frames before cf=14200):

```
server: slot=5 sim_frame=14186 target_frame=14187 vc_frame=14186 lcg=b08d8270 bytes=09,01,a3,0d
server: slot=5 sim_frame=14186 target_frame=14187 vc_frame=14186 lcg=b08d8270 bytes=15,00,00,00,00,39,0e,e4..

obs:    slot=5 sim_frame=14186 target_frame=14187 vc_frame=14185 lcg=b08d8270 bytes=09,01,a3,0d
obs:    slot=5 sim_frame=14186 target_frame=14187 vc_frame=14185 lcg=b08d8270 bytes=15,00,00,00,00,39,0e,e4..
```

Identical action bytes, identical pre-apply lcg, identical
sim_frame/target_frame — but `vc_frame` is 1 lower on the observer.
That's the one asymmetry visible pre-divergence.

`vc_frame` decorates the log; it feeds `target_frame = vc_frame + latency`
at schedule time, and gates when scheduled_actions execute. It does
NOT flow into `next_frame()`. So the 1-tick vc_frame offset should
not affect sim outcome... unless there's a code path I've missed.

### Late joiners (dave, eve) — separate bug

Both diverge INSTANTLY on their first shared TICK frame:
- dave joined at cf=18234, diverged at cf=18234 (n0 mismatched: 62 vs 55)
- eve joined at cf=20215, diverged at cf=20215 (n0 mismatched: 75 vs 56)

Their catchup replay produces a starting state that differs from
what the server holds live. All owners' hashes are different. This
is likely a distinct bug from the slot-5 quiet-tick divergence.

Preserved: `docs/repro_r7_2026-07-12/runs/repro_r7_owners/critical/server_cf18200-18300.tsv`
and `obs_dave_cf18200-18300.tsv`.

---

## Prior runs (context)

- **Run 1** (per-frame LCG only, `/tmp/repro_r7_bc`, ~11:07):
  bob+frank diverge at cf=14545 (identical wrong lcg 11d78c63),
  alice at 43472, dave/eve immediately.
- **Run 2** (state-hash `nu/ht/od/tp` global, `/tmp/repro_r7_bcd`, ~11:17):
  bob and frank clean; alice diverges at cf=14213 (ht split 167
  frames before lcg); dave/eve immediately.
- **Run 3** (per-owner hashes, `/tmp/repro_r7_owners`, ~11:35):
  all 4 early joiners diverge at cf=14200 on slot-5 hash.

Frame varies run to run (14200/14213/14545/43472) — some
scheduler-order race component in the initial trigger. Within a
run, all early joiners agree on the SAME wrong state at the SAME
frame.

---

## Reproducibility

Deterministic within a run: FAIL_REAL is stable across analysis
re-runs of the same sync-log. Non-deterministic across runs: same
script and same launch order produce different divergence frames.
Suggests the trigger involves message-arrival timing.

Reproducing takes ~9 minutes end-to-end (240s launch stagger +
300s play + shutdown/analysis). FAIL rate is 100% for late joiners
(dave/eve); ~50-100% for early joiners.

---

## Ruled out

1. **AGENT_APPLY byte corruption** — logs identical on both sides.
   Ruled out by `AGENT_APPLY` byte-diff.
2. **Race-override desync** — server ships pre-random-pick races in
   catchup bundle (commit 66f3305). Verified by identical
   `GAME_START initial_rand` across all peers.
3. **Race between observers** — same wrong hash on all early-joined
   observers this run. Not a race among peers.
4. **Random unit spawn ordering** — nu (unit count) matches at
   divergence; no birth/death event.
5. **iscript / order machine mismatch** — od hash is part of the h_i
   per-owner hash which splits; od could still be the smoking gun.
   Need per-unit breakdown to confirm.

---

## Not yet ruled out (open questions for the tester)

### Q1 — WHICH slot-5 unit diverges first?

The h5 hash split at cf=14200 tells us "one of 11 slot-5 units
differs" but not WHICH one, or WHICH field.

**Diagnostic to add**: dump per-unit state at cf=14199 and cf=14200,
both sides, for all slot-5 units:

```cpp
// in sync_functions::next_frame(), gate on current_frame in [14199, 14200]
for (unit_t* u : ptr(this->st.visible_units)) {
    if (u->owner != 5) continue;
    a_string body = format(
        "UNIT\tidx=%u\towner=%d\ttype=%d\thp=%d\tsp=%d"
        "\tx=%d\ty=%d\ted_x=%d\ted_y=%d"
        "\torder=%d\tstate=%d\ttimer=%d"
        "\tanim_frame=%d\theading=%d",
        u->index, u->owner, u->unit_type ? u->unit_type->id : 0,
        u->hp.raw_value, u->shield_points.raw_value,
        u->position.x, u->position.y,
        u->exact_position.x.raw_value, u->exact_position.y.raw_value,
        u->order_type ? u->order_type->id : 0,
        u->order_state, u->main_order_timer,
        u->sprite ? u->sprite->main_image->frame_index : 0,
        u->heading.raw_value);
    sync_log_line(sync_st, side, body);
}
```

`diff` server's UNIT lines vs alice's at cf=14200 — the ONE unit
whose field differs is the divergence source.

### Q2 — What triggers the first micro-divergence?

If it's position/HP → look at movement or damage code between
frame 14186 (last slot-5 action) and 14200. Slot 5 is the terran
in position 5 (frank); the action was SELECT + ATTACK_MOVE. The
unit(s) may be mid-movement, mid-turn, or entering combat range.

If it's order_state/timer → an iscript animation timer advanced
differently. Investigate `iscript_run_to_idle` or per-frame
iscript step functions.

### Q3 — vc_frame off-by-1 — decoration or causal?

Server's `vc_frame` in AGENT_APPLY is consistently 1 higher than
observer's for the same action. Reason: server bumps `vc->frame`
per tick for all virtual clients in `server/main.cpp:600-606`;
observer only updates on incoming `id_agent_action`. But
`vc->frame` only feeds `target_frame = vc->frame + latency` at
scheduling time, and gates when scheduled_actions fire — it does
NOT affect `next_frame()`. Confirm by grep on `client->frame` /
`vc->frame` in sync.h:

```
sync.h:356   scheduled_actions.front().frame (executes when sync_frame==frame)
sync.h:649   target_frame = client->frame + latency
sync.h:816   client->frame = wire (from id_client_frame)
sync.h:1044  vc->frame = server_frame (id_agent_action arrival)
sync.h:1358  timeout check (>60s)
```

None of those flow into sim state. Should be safe to leave.

### Q4 — Late-joiner catchup: why does it produce a different state?

Dave and eve diverge on FIRST shared TICK. Their catchup path
must produce a state that differs from server's live state. Suspect:

- **AGENT_RECV ordering** relative to sim ticks: when a late
  observer receives the catchup bundle, it fast-forwards by
  replaying scheduled_actions from the bundle. If the server's
  live state contains subframe motion that isn't in the bundle
  (only the actions are, not the resulting positions), the
  observer will apply the same actions but the sim state may
  have already progressed differently on the server (if any
  non-action code drifted during the pre-join window).

If Q1 confirms subframe drift in slot 5, then Q4 is downstream of
Q1: same bug fires during the pre-join replay, so late joiners
never had a matching state.

### Q5 — Why isn't the same divergence hitting 2-player games?

Prior 1-hour soak on 2/4/8-player maps (task #113) passed 10/10.
Only 6-player New Gettysburg fails so far. Two variables to test:
- Player count: retry (2)Bottleneck etc. with 6 P-T-P-T-P-T mix.
- Map size / initial unit density: New Gettysburg's initial
  neutral units + wide layout differ from other maps.

---

## Files that will be relevant to the fix

- `sync.h` — all diagnostic instrumentation lives here (search
  for `TICK\t`, `LCG_TICK`, `AGENT_APPLY`, `AGENT_RECV`,
  `execute_scheduled_actions`). Also the catchup replay path.
- `bwgame.h` — the sim itself. `next_frame()` at line 22363
  is the sim tick entry. Movement/orders/iscript live below.
- `server/main.cpp` — server main loop, cmd_queue drain,
  per-tick vc->frame bump (~lines 600-606).
- `ui/observer.cpp` / `wasm/observer_wasm.cpp` — observer main
  loops. Not likely the source — they just drive sync_functions.
- `scripts/repro_round7.sh` — repro launcher.
- `scripts/analyze_state_divergence.py` — per-owner analyzer.

---

## Immediate next-step for a tester agent

1. Rebuild:
   ```
   cd build_srv && make -j4 openbw_server openbw_observer
   ```

2. Run repro:
   ```
   rm -rf /tmp/repro_r7_next
   OUT=/tmp/repro_r7_next scripts/repro_round7.sh
   ```

3. Analyze:
   ```
   for f in obs_alice obs_bob obs_carol obs_dave obs_eve obs_frank; do
     echo "=== $f ==="
     python3 scripts/analyze_state_divergence.py \
       /tmp/repro_r7_next/server.sync \
       /tmp/repro_r7_next/${f}.sync 2>&1 | head -30
   done
   ```

4. Identify the earliest divergence frame (call it `cf_D`) and the
   slot whose h_i first splits.

5. Add the UNIT-level dump from Q1 above, rebuild, re-run, and
   `diff` the UNIT lines between server and one failing observer
   at cf_D-1 and cf_D. Report the field(s) that differ.

6. If (hp+position) differs: instrument
   `unit_next_movement_frame` and `execute_movement` in bwgame.h
   to log per-tick input state; look for the per-tick input that
   differs between sides.

7. If (order/state/timer) differs: instrument the iscript step
   for that unit's animation script.

The goal is to find the specific state field and the specific
call site that produces a different value on server vs observer
during a quiet tick with no incoming action. That's the leak.

---

## Related memory files (for AI agents)

- `project_syncbreaker.md` — SyncBreaker taxonomy and prior fixes.
- `project_syncbreaker5_findings.md` — Run-1 findings (bob/frank
  identical wrong lcg).
- `feedback_sync_log_off_by_one.md` — small INVENTORY drift is a
  logging-order artifact, not divergence.

---

## Related open tasks

- #114 SyncBreaker #5 (this)
- #115 Command queue drain slot-serial (DoS surface)
- #116 Map loader unknown trigger action 12 (some campaign maps)
- #117 SyncBreaker #5 root-cause investigation
- #118 e5a15f4 refinement (preserve slab fast path)
- #119 State-hash TICK trace (this bug)
