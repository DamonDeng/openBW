# SyncBreaker #5 repro summary — 2026-07-12

> **Handoff:** later same-day investigation is captured in
> `handoff_syncbreaker5.md` (this directory). Read that first if
> you're picking up the case; the section below is the original
> morning-of-investigation snapshot.


## Repro config
- Script: `scripts/repro_round7.sh`
- Map: `(6)New Gettysburg.scm`
- Races: protoss, protoss, terran, terran, terran, terran
- Game speed: 10ms/frame (100 FPS)
- Play window: 300s
- Launch order: `[O:1 A:0 O:0 O:5 A:2 A:5 O:2 A:4 A:1 O:3 O:4 A:3]`

## Result (with a566acc "guard against corrupt action" observer fix)

| observer | matched | min_only | real | verdict |
|---|---|---|---|---|
| alice  | 318 | 0 | 254 | FAIL_REAL |
| bob    | 346 | 0 | 254 | FAIL_REAL |
| carol  | 266 | 0 |   0 | PASS |
| dave   | 226 | 0 |   0 | PASS |
| eve    | 212 | 0 |   0 | PASS |
| frank  | 306 | 1 |  61 | FAIL_REAL |

- Server ran to frame 52011 (full 300s @ speed=10).
- Zero BAD_ACTION events fired -- action decode succeeded on all observers.

## Divergence signature

At the first divergence frame for each failing observer (frame 14100
for alice/bob, 42900 for frank):

- min, gas, completed unit counts, in-progress unit counts:
  **identical between observer and server**.
- lcg_rand_state: **different**.

Example (alice at frame 14100 slot 0):
```
SERVER:  min=176 gas=1268 lcg=580324b1 completed=64:33,65:5,... in_progress=64:1,65:1,...
alice:   min=176 gas=1268 lcg=b4a8e7f0 completed=64:33,65:5,... in_progress=64:1,65:1,...
```

lcg diverged BEFORE unit counts. Alice and bob have IDENTICAL divergent
lcg (`b4a8e7f0`); frank later diverges to a different lcg. So multiple
observers can end up on the same wrong lcg -- which means the divergence
event is a SIM step, not a per-observer accident.

## Diagnostic conclusions

- Observers receive byte-identical AGENT_APPLY events at identical
  frames (server AGENT_APPLY log vs each observer's log matches
  perfectly for the window 13800..14100 leading up to alice's
  divergence).
- Yet lcg on server advances differently than on observers over that
  same 300-frame window.
- Something the server sim does that consumes lcg is NOT done by
  observers (or vice versa) -- but that something is not visible as
  an AGENT_APPLY event.

Suspect: some server-side helper called from the sim thread that
touches state and consumes lcg. Candidates to audit:

- `broadcast_agent_action`: doesn't obviously call lcg. Check.
- `funcs.schedule_action(vc, data, size)`: shared between server and
  observer -- same code path. Should not consume lcg differently.
- `funcs.execute_scheduled_actions`: also shared, same code path.
- Command-queue drain: does NOT call sim. Just copies bytes.

Could be an out-of-order execute path: server-side, an agent's
command arrives via HTTP queue and gets applied at frame N. Observer
gets the same broadcast id_agent_action for frame N -- but the
observer's `client->frame` counter for that virtual client may differ
from the server's `vc->frame` at scheduling time, causing the observer
to schedule at target_frame = client->frame + latency (different).
If off by one, the action applies one frame later on the observer,
inside next_frame's per-frame loop where lcg fires for unit AI --
resulting in different lcg after that frame.

Next debug step (planned): add per-tick lcg snapshot log alongside
the tick heartbeat. If server logs `frame=N lcg=X` and observer logs
`frame=N lcg=Y` for some N, we've localized the divergence to a
specific frame.
