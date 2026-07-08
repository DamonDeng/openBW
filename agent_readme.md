# openBW as a Server-Client Agent Sim — Plan

This document captures the architecture, decisions, and task breakdown for
turning openBW into a server-client style application that supports LLM agents
playing StarCraft 1 with humans observing.

## Goal

Leverage openBW's high-performance, deterministic SC1 replication to build a
stack where:

- **N LLM agents** (up to 8, one per player slot) send commands via RPC.
- **Human viewers** watch what their agent (or other agents) are doing in
  near-real-time through an SDL2 client.
- A **central process** (openBW server) is the sole authority for game state,
  so no bit-for-bit determinism ceremony across multiple sim processes.

Not multiplayer BW. Not a general RTS server. Specifically: agent-vs-agent SC1
with human viewers.

## Architecture

```
┌─────────────── openBW Server Process (single binary) ────────────────────────┐
│                                                                              │
│  ┌── Main sim thread (single-threaded, owns bwgame::state) ──────────┐       │
│  │  Fixed-rate loop (24 FPS BW):                                     │       │
│  │    frame = current_frame + 1                                      │       │
│  │    for slot in 0..7:                                              │       │
│  │      while (cmd = agent_queue[slot].try_pop()):                   │       │
│  │        execute_action(slot, cmd.bytes)                            │       │
│  │        replay_saver.add_action(frame, slot, cmd.bytes)            │       │
│  │    sync_functions::next_frame()   # advances sim & broadcasts     │       │
│  └───────────────────────────────────────────────────────────────────┘       │
│                ▲                    ▲                       │                │
│                │                    │                       │                │
│  ┌── Sidecar (thread in same process) ────┐        ┌────────▼──────────┐     │
│  │  HTTP server: control plane            │        │  sync.h TCP server│     │
│  │    /game/create /start /pause /reset   │        │  observer peers   │     │
│  │    /game/{id}/join → {token, ws_url}   │        │  (player_slot=-1) │     │
│  │  WS server: per-agent gameplay         │        │                   │     │
│  │    on message → agent_queue[slot].push │        │                   │     │
│  │    on observe → snapshot state         │        │                   │     │
│  └────────────────────────────────────────┘        └───────────────────┘     │
│         ▲                      ▲                             │               │
└─────────┼──────────────────────┼─────────────────────────────┼───────────────┘
          │HTTP                  │WebSocket                    │TCP (sync.h)
          │                      │                             │
    Control clients        8× LLM Agents               N× Observer clients
    (start/pause/reset)    (act + observe, pull)       (openBW SDL2, read-only)
```

### Server side

- **One authoritative sim** running in a single process. All game state lives
  in `bwgame::state` on the sim thread.
- **Sidecar as a thread**, not a separate process. Simpler to build/debug/ship;
  trivially upgradable to an out-of-process sidecar by putting a socket between
  the RPC handler and the queue.
- **HTTP for control plane**, WebSocket for gameplay. HTTP endpoints handle
  game lifecycle (create/start/pause/reset/join). Once an agent joins, it
  opens a WebSocket that stays live for the duration of the game.
- **Observer peers use unmodified sync.h**, joining with `player_slot = -1`.
  They run their own full sim locally and stay in sync via the existing
  scheduled-action broadcast, but never submit commands themselves.

### Agent side

- **Pull-based observation**: agent calls `observe()` when ready to think.
  Server returns a snapshot of everything visible to that slot (respecting fog
  of war).
- **Fire-and-forget commands**: agent sends `act(command)` over WebSocket.
  Server acknowledges with the frame the command will apply on
  (`current_frame + 1`).
- **Sim never waits for agents.** If a slot's queue is empty at tick time, no
  action is applied for that slot this frame — its units continue whatever
  they were doing (mining, moving, attacking). This matches how BW handles
  slow human input.

### Viewer side

- Existing `ui/sdl2.cpp` client, patched with an `--observer <host:port>`
  flag.
- Renders locally from a full sim running in lockstep with the server via
  `sync.h`.
- Free-camera scroll, zoom, unit selection, fog-of-war toggle all work
  unchanged — they live in `ui_functions`, not in `bwgame::state`.
- Cannot issue unit commands. Camera-only control.

## Design invariants

These are correctness guarantees the code must enforce:

1. **Sim thread is single-threaded and owns `bwgame::state`.** Nothing else
   ever touches sim state directly.
2. **Command queue is per-slot.** One queue per player slot, populated by the
   WebSocket handler threads.
3. **Commands drain in slot order (0 → 7), FIFO within a slot.** Deterministic
   tie-breaking when multiple agents submit within the same tick window.
4. **Sim thread never blocks on agents or observers.** Producers can be slow;
   consumers can lag. Neither can stall the tick.
5. **Observations are snapshots, not pointers.** `observe(slot)` copies out
   visible state and returns; state can advance freely while the agent
   reasons.
6. **Server assigns frame numbers to commands on the sim thread only.** This
   is what makes the action log — and therefore observer replays — bit-exact.

## Why this over alternatives

Three architectures were considered:

| | Custom server-authoritative (rejected) | Sidecar-over-sync.h (rejected) | **Observation-mode over sync.h (chosen)** |
|---|---|---|---|
| Reuses sync.h peer code | ❌ | ✅ | ✅ |
| Full sims per game | 1 server + N viewers | 9 + N (host + 8 sidecars + N viewers) | 1 server + N observers |
| Handles slow LLMs | ✅ | ✅ | ✅ |
| Reuses existing SDL2 client | Requires porting | Requires porting | ✅ mostly unmodified |
| Reuses replay format for late-join | Have to build | Have to build | ✅ already there |
| New code to write | Lots | Lots + heavy runtime cost | Minimal |

The observation-mode plan works because openBW is already 95% of the way
there:

- `sync.h` already has an authoritative-peer + follower-peers model.
- `client_t::player_slot` defaults to `-1`, which naturally means "observer."
- Camera state is UI-local, not sim state — spectator scroll is free.
- `replay_saver.h` already serializes exactly the action log observers need.

Two small patches to `sync.h` unlock the whole thing:

1. Skip `player_slot == -1` clients in `all_clients_in_sync()` so observers
   can't stall the game.
2. Bump the per-observer action buffer size a few multiples so brief network
   hiccups don't overflow.

## Reuse map

| Piece                             | Source                                       | New work                                    |
|-----------------------------------|----------------------------------------------|---------------------------------------------|
| Deterministic sim core            | `bwgame.h`, `actions.h`                      | none — reuse as-is                          |
| Command shape (order/target/unit) | `actions.h` encoders + `mini-openbwapi/`     | wrap in JSON/WS message                     |
| Observation shape                 | `mini-openbwapi/` getters                    | wrap in JSON snapshot                       |
| Headless game loop                | `ui/gfxtest.cpp::main_t::update`             | copy → server binary, strip UI              |
| Frame-stamped action log          | `replay_saver.h`                             | reuse for late-join fast-forward            |
| Peer sync protocol                | `sync.h` + `sync_server_asio_tcp.h`          | 2 small patches (observer-skip, buffer)     |
| Viewer UI                         | `ui/ui.h` + `ui/sdl2.cpp`                    | add `--observer` flag, disable input verbs  |
| Control API                       | —                                            | new: cpp-httplib or similar                 |
| WebSocket gameplay                | —                                            | new                                         |

## Task list & build order

Tasks are tracked in the harness task system (see current tasks with
`TaskList`). Build order — each is a natural demo milestone:

1. ✅ **#6 Patch sync.h** — done. `all_clients_in_sync` skips observer peers.
2. ✅ **#7 Server skeleton** + ✅ **#12 Observer client** — done. Two
   binaries `openbw_server` and `openbw_observer` build via a top-level
   CMakeLists.txt. Verified end-to-end: server binds TCP, waits for first
   observer to connect, starts the game, ticks at 24 FPS; observer opens
   an SDL2 window and syncs.
3. **#8 Command queue** + **#10 WebSocket agents** — first agent plug-in
   point. Hardcoded fake agent sends "build SCV"; observer sees the SCV.
4. **#11 Observation serializer** — closes the LLM loop. Agent calls
   `observe()`, gets JSON, sends `act(train_scv)`, sees updated resources.
5. **#9 HTTP control API** — makes it operable. Create/start/reset via curl.
6. **#13 Late-join for observers** — v1.1 polish; not needed for MVP.
   Currently the server *requires* an observer to connect before start_game,
   because sync.h refuses new peers once game_started == true.
7. **#14 Multi-game support** — when you want to run tournaments.

## How to run the observation-mode demo

Prereqs: `original_resources/` contains `StarDat.mpq`, `BrooDat.mpq`,
`Patch_rt.mpq` (SC1 assets, gitignored), and a map file (e.g. copy any
`*.scm` from your SC install into `original_resources/`).

```bash
# Configure & build (once)
cmake -S . -B build_srv
cmake --build build_srv -j

# Terminal 1: server
./build_srv/server/openbw_server \
  --map "original_resources/(2)Bottleneck.scm" \
  --data-path original_resources

# Terminal 2: observer (once server is listening)
./build_srv/ui/openbw_observer \
  --map "original_resources/(2)Bottleneck.scm" \
  --data-path original_resources \
  --server 127.0.0.1:6112
```

Server logs frame count + connected observer count once per second.
Observer opens an SDL2 window showing the map.

## Open questions to revisit later

- **JSON vs protobuf for wire messages.** JSON is fine at LLM cadence
  (seconds). Switch if observations ever need to be many-per-second.
- **Command validation error UX.** Invalid commands (unit dead, insufficient
  resources) drop silently in openBW today. For LLM debugging we probably
  want structured error replies. Decide when writing task #10.
- **Fog of war for observers.** Full-vision by default (spectator mode), but
  optionally per-player view for demo/streaming. `ui_functions` already has
  the switch.
- **Multi-game concurrency model.** Thread-per-room vs pool-of-workers.
  Decide when task #14 lands.

## References

- `sync.h` lines 191–205 (`execute_scheduled_actions`), 949–956
  (`all_clients_in_sync`, the observer-stall bug), 279 (per-client latency
  scheduling).
- `ui/gfxtest.cpp::main_t` — reference for the sim step loop the server will
  reuse.
- `mini-openbwapi/` — reference for the command/observation surface the RPC
  layer wraps.
- `replay_saver.h` / `replay.h` — action-log serialization reused for
  observer late-join.
