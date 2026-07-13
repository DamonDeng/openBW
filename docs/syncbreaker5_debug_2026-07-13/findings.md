# SyncBreaker #5 — new findings from 2-player P v T repro (2026-07-13)

## What we did

Simsc_app Day 2 (new Qt observer) surfaced a client-visible symptom:
window shows only a Nexus + a Pylon while agents report `bldgs=3
pending=6`. First hypothesis: Qt bug. Ruled out — swapping in the
native SDL observer with identical server config produces the same
divergence. Both observers are downstream of a live sim bug.

Config (both sides):
- Map: `original_resources/(2)Bottleneck.scm`
- 2 players (T v P) driven by t_agent_v5 + p_agent_v4
- Observer role: no-fog (`sk-w`)
- Server: `--game-speed 10 --wait-observers 1 --sync-log`
- Observer: same map, `--sync-log`

Two runs preserved under this directory:
- `server.sync` + `sdl.sync`: no `--fixed-initial-rand`; both sides
  arrive at same `initial_rand=473fd28d`.

## Symptom, precisely

**Not** a straight LCG divergence. The observer's LCG sequence is
the SAME as the server's — just delayed by ~11 sim frames. Table
from the preserved sync-logs:

| server cf | server lcg | observer cf | observer lcg |
|-----------|------------|-------------|--------------|
| 13        | 68a7bd25   | 24          | 68a7bd25     |
| 21        | 6a5d6eaa   | 27          | 6a5d6eaa     |
| 22        | 0ed3b533   | 29          | 0ed3b533     |

Every LCG step server takes, observer takes the same step ~11 frames
later. So the RNG chain is not corrupted — it's *phase-shifted*.

At speed=10 (10 ms/frame) that's 110 ms of drift. Enough that:
- SCV pathfinding decisions land on different frames
- Building placement lcg calls on the observer walk through a state
  that server was in ~11 frames ago (units in different positions)
- Once one placement resolves to different (x,y) on observer vs
  server, the two sims fork.

## First shared TICK from observer's catchup

```
S  19  TICK  cf=19  lcg=e4690e1b  n=5,5,...  h0=bf6b1d21  h1=3c5620b0  vcs=18,18,-,...
O   0  TICK  cf=19  lcg=c4ef8d32  n=5,5,...  h0=bf6b1d21  h1=3c5620b0  vcs=-,-,-,...
```

- Unit counts + per-owner hashes **identical** at cf=19 (game state
  matches).
- LCG state differs (`e4690e1b` vs `c4ef8d32`) — observer already
  advanced past this point in the LCG chain during its catchup
  replay.
- `vcs=18,18,...` on server (both virtual clients at frame 18);
  `vcs=-,-,...` on observer — **observer has no virtual clients
  registered**.

## Root cause hypothesis

The observer's `handle_catchup` (sync.h:1092) runs a fast-forward
loop over `catchup_action_bytes` + `next_frame()` calls, up to the
server's `target_frame`. This loop consumes LCG somewhat differently
than the server's live sim did over the same frames:

1. The action-application ordering may differ from what the server
   ran live (server: per-slot round-robin via `execute_scheduled_
   actions`; observer catchup: linear scan of the replay bytes).
2. `action_functions::next_frame()` in the catchup loop runs
   WITHOUT virtual-client bookkeeping — server's live tick bumps
   `vc->frame` per client each tick, observer's catchup doesn't.
3. Observer's start_game runs before catchup (`id_start_game`
   arrives first, `start_game(seed)` XORs UIDs). Then `handle_
   catchup` sees `game_started=true` and skips `start_game_local`.
   RNG state at start-of-catchup is already off from server's.

## Reproduction snippet

Kill everything, then:

```bash
rm -f /tmp/sb-debug/*.log
./build_srv/server/openbw_server \
    --data-path original_resources \
    --map 'original_resources/(2)Bottleneck.scm' \
    --user 'terran_p:sk-t:player:0' \
    --user 'protoss_p:sk-p:player:1' \
    --user 'demo_obs:sk-w:observer' \
    --race 0=terran --race 1=protoss \
    --game-speed 10 --obs-port 6114 --wait-observers 1 \
    --sync-log /tmp/sb-debug/server.sync &
env SDL_VIDEODRIVER=dummy ./build_srv/ui/openbw_observer \
    --data-path original_resources \
    --map 'original_resources/(2)Bottleneck.scm' \
    --server 127.0.0.1:6114 --api-key sk-w \
    --race 0=terran --race 1=protoss \
    --sync-log /tmp/sb-debug/sdl.sync &
# wait for game start, then attach agents
python3 -m python_agent.agents.t_agent_v5 sk-t --host 127.0.0.1 --port 6113 --interval-sec 0.5 --base-target 2 &
python3 -m python_agent.agents.p_agent_v4 sk-p --host 127.0.0.1 --port 6113 --interval-sec 0.5 --base-target 2 &
sleep 40
pkill -f openbw_server; pkill -f openbw_observer; pkill -f python_agent
grep LCG_TICK /tmp/sb-debug/server.sync | head -30
grep LCG_TICK /tmp/sb-debug/sdl.sync    | head -30
```

Compare LCG chains — they'll match with an ~11-frame offset.

## Next steps

1. Instrument `handle_catchup` to log each `action_functions::
   next_frame()` call's pre/post LCG state, and each
   `execute_actions` call's action bytes + pre/post LCG. Compare
   against server's live per-frame LCG_TICK for frames 0..target.
2. Check whether virtual-client bookkeeping (`vc->frame` bumps,
   `execute_scheduled_actions` accounting) on the server consumes
   LCG that the observer's action_functions::execute_actions does
   not.
3. If (2) is confirmed: either replay `next_frame` through the
   same sync-layer path on the observer, or precompute the
   `sync(server)` LCG work on the server side and ship it in the
   catchup bundle.

## Attribution

This debug session started because the Qt spectator (Day 2 of the
Qt wrapper work, commit `0624254`) rendered a visibly-stuck game.
The Qt client is not the cause — the divergence reproduces on the
SDL native observer with identical wire behavior. But the Qt client
is a nicer reproduction vehicle than a headless log-diff: you can
see the divergent state directly on screen.
