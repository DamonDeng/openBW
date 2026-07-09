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

Small patches to `sync.h` unlocked the whole thing (implemented across
the tasks above):

1. Skip `player_slot == -1` (observers) and `h == nullptr` (virtual
   clients) in `all_clients_in_sync()` so neither can stall the sim.
2. Relax the pre-game peer cap to count only local + real network
   peers with a socket, letting N observers + K virtual slot clients
   coexist.
3. Late-join gate removed from `on_new_client`; `id_client_uid`
   handler distinguishes pre-game (reset the lobby) vs mid-game
   (bootstrap the new peer without disturbing the running sim).
4. New wire messages: `id_auth` (client key → server verify), `id_assign_perspective` (server → client slot), `id_catchup_data`
   (server → late joiner action-log bundle), `id_agent_action`
   (server → observers live agent commands).

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
3. ✅ **#15 Multi-observer support** — done. sync.h had a pre-game peer
   cap of 2 (BW 1v1 lobby assumption); relaxed to count only player-slot
   peers, so observers are unlimited. Server has --wait-observers N.
4. ✅ **#17 Auth: user registry** — done. users.json loaded at startup;
   SHA-256 hashes stored, plaintext discarded. server::verify(key) →
   user_t*. Reused across HTTP / WebSocket / sync.h.
5. ✅ **#18 Auth: sync.h id_auth handshake** — done. Observer sends its
   API key right after greeting. Server verifies via callback and
   stashes user pointer on client_t::auth_user.
6. ✅ **#19 Server-assigned perspective + fog of war** — done. Server
   sends id_assign_perspective(slot) based on the authenticated user's
   assigned_slot. Observer renders with per-player visibility filter
   (sprites gated by visibility_flags, tiles darkened via dark_pcx
   row 7 for fog, black for unexplored). Fixed a startup race where
   observer 3+ could stall inside SDL2 window creation and miss its
   async_connect. Fixed the fog tint (was using light_pcx[6] which
   produced a green cast on jungle tilesets).
7. ✅ **#13 Late-join for observers** — done. Server starts immediately
   (--wait-observers default is 0); observers can connect any time,
   quit and reconnect freely. New id_catchup_data message ships the
   replay_saver history to any late joiner; observer runs
   start_game_local + a fast-forward loop of execute_actions +
   next_frame to align its sim with the server's current frame.
8. ✅ **#8 Command queue** + ✅ **#10 WebSocket agents** — done. Agents
   connect over WS on port 6113 (default), authenticate with the same
   API key. Command JSON is encoded to BW action bytes and dropped
   into a per-slot mutex-guarded queue; the sim thread drains slots
   in order (0 → 7, FIFO within slot) each tick and both
   schedule_actions locally AND broadcasts via id_agent_action so
   already-connected observers stay in sync. Verbs supported: move,
   attack, stop, train, build. Server registers 8 virtual sync
   client_t entries (one per active slot, no socket, has_auth=true,
   game_started=true) so execute_scheduled_actions actually applies
   the queued bytes.
9. ✅ **#11 Observation serializer** — done. observe() over WS returns
   a JSON snapshot: resources, own units, visible enemies (fog
   respected), neutrals (mineral fields / geysers), and optional
   static map_info. Threaded correctly: WS handler queues a request,
   sim thread serializes on its own thread, response is posted back
   to the WS io_service for delivery. targets parameter filters the
   payload (units / enemies / resources / map_info / all).
10. **#9 HTTP control API** — operator plane. Reuses auth via Bearer.
11. **#14 Multi-game support** — when you want to run tournaments.

## Identity model

Every actor (agent, observer, operator) has an API key. On startup, the
server loads a users.json file:

```json
{
  "users": [
    {"alias": "alice", "api_key": "sk-...", "slot": 0},
    {"alias": "bob",   "api_key": "sk-...", "slot": 1},
    {"alias": "spectator", "api_key": "sk-...", "role": "observer"},
    {"alias": "admin", "api_key": "sk-...", "role": "admin"}
  ]
}
```

Server hashes each api_key at load time and discards the plaintext.
Actors present their key over their transport:

- **HTTP**: `Authorization: Bearer <key>`
- **WebSocket**: upgrade query string or subprotocol
- **sync.h**: new `id_auth` message sent before `id_client_uid`

One `server::verify(key) → user*` function backs all three. A user's
`assigned_slot` drives:
- WebSocket agent connections → what slot they can control
- Observer connections → what perspective they see (fog of war)

Keys are demo-cleartext for now; TLS/wss is a deployment concern for a
future task.

## How to run the demo

Prereqs: `original_resources/` contains `StarDat.mpq`, `BrooDat.mpq`,
`Patch_rt.mpq` (SC1 assets, gitignored), and a map file (e.g. copy any
`*.scm` from your SC install into `original_resources/`).
`test_resources/users.json` has API keys for the built-in test users
(see `test_resources/test_guidance.md` for copy-paste commands).

```bash
# Configure & build (once)
cmake -S . -B build_srv
cmake --build build_srv -j

# Terminal 1: server (starts immediately, late joiners welcome)
./build_srv/server/openbw_server \
  --map "original_resources/(2)Bottleneck.scm" \
  --data-path original_resources \
  --users test_resources/users.json
# Optional: override races so both slots don't spawn the map's default
# race. `--race N=RACE` is per-slot and repeatable:
#   --race 0=terran --race 1=zerg
# See test_resources/test_guidance.md for the full CLI reference.

# Terminal 2: observer as alice (slot 0)
./build_srv/ui/openbw_observer \
  --map "original_resources/(2)Bottleneck.scm" \
  --data-path original_resources \
  --server 127.0.0.1:6112 \
  --api-key <alice's key from test_resources/users.json>

# Terminal 3: agent as alice, sending a move command via WebSocket
python3 <<'PY'
import asyncio, json, websockets
async def m():
    key = "<alice's key>"
    async with websockets.connect(f"ws://127.0.0.1:6113/agent?key={key}") as ws:
        print(await ws.recv())                                    # welcome
        await ws.send(json.dumps({"type":"observe","id":"o1"}))
        obs = json.loads(await ws.recv())                         # observation
        u = next(x for x in obs["units"] if not x.get("building"))
        await ws.send(json.dumps({"type":"cmd","id":"m1",
            "cmd":{"verb":"move","unit":u["unit_id"],"x":2000,"y":2000}}))
        print(await ws.recv())                                    # ack
asyncio.run(m())
PY
```

The observer window shows the SCV walking. The full copy-paste playbook
for 2-player and 8-player maps lives in `test_resources/test_guidance.md`.

## Agent WebSocket protocol

Agents connect to `ws://<server>:6113/agent?key=<api_key>`. Auth is
verified from the users.json registry; connection is refused if the key
is unknown or the user's role isn't `player`. On accept, the server
sends `{"type":"welcome","slot":N,"current_frame":F}`.

### Client → server

```jsonc
// Ask for a snapshot of what the agent's slot can see.
{"type":"observe", "id":"o1", "targets":["units","enemies","resources","map_info"]}
// targets is optional. Default = ["units","enemies","resources"].
// "all" is shorthand for everything.

// Issue a unit command. Verbs: move, attack, stop, train, build.
{"type":"cmd", "id":"m1", "cmd":{"verb":"move","unit":3684,"x":1024,"y":768}}
{"type":"cmd", "id":"a1", "cmd":{"verb":"attack","unit":3684,"target_unit":3699,"x":0,"y":0}}
{"type":"cmd", "id":"s1", "cmd":{"verb":"stop","unit":3684}}
{"type":"cmd", "id":"t1", "cmd":{"verb":"train","unit":3720,"unit_type":7}}
{"type":"cmd", "id":"b1", "cmd":{"verb":"build","unit":3684,"unit_type":106,"tile_x":24,"tile_y":30}}
```

`unit_id` is the raw 16-bit BW unit id (includes generation bits) — get
it from an `observe()` response, not by counting.

### Server → client

```jsonc
{"type":"welcome",     "slot":0, "current_frame":42}
{"type":"ack",         "id":"m1", "queued_at_frame":43}
{"type":"error",       "id":"m1", "message":"..."}
{"type":"observation", "id":"o1", "slot":0, "current_frame":123,
 "resources": {"minerals":50,"gas":0,"supply_used":8,"supply_max":20,...},
 "units":     [{"unit_id":3684, "type":64, "x":3832, "y":2440,
                "hp":20, "hp_max":20, "shields":20, "shields_max":20,
                "order":3, "completed":true}, ...],
 "enemies":   [...],
 "neutrals":  [...],                                          // mineral fields, geysers
 "map_info":  {"tile_width":128, "tile_height":128, ...}}     // only if requested
```

### Threading & determinism

- Sim thread is single-threaded and owns `bwgame::state`. Never touched
  by WS handlers.
- Commands: WS handler encodes JSON → BW action bytes → pushes into a
  per-slot mutex-guarded deque. Sim thread drains all 8 slots in order
  each tick, `schedule_action`s on the slot's virtual sync client (so
  `execute_scheduled_actions` dispatches them), and broadcasts each
  action to every connected observer via `id_agent_action` so live
  observers stay frame-for-frame with the server.
- Observations: WS handler queues a request; sim thread serializes on
  its own thread; response is posted back to the ws_server's io_service
  for delivery.
- Determinism: server assigns the frame each command applies on
  (`current_frame + 1`). Every observer's local sim, whether live or
  fast-forwarding via id_catchup_data, sees the same action stream in
  the same order.

## Open questions to revisit later

- **JSON vs protobuf for wire messages.** JSON is fine at LLM cadence
  (seconds). Switch if observations ever need to be many-per-second.
- **Structured command errors.** Invalid commands (unit dead,
  insufficient resources) currently drop silently inside
  `read_action_*`. LLMs would benefit from an error reply per rejected
  command.
- **Unit type names in observations.** Currently emit integer `type`
  only. A small enum → string lookup table (228 entries) would help
  LLMs but adds tokens; skip until an LLM struggles with it.
- **Event push (unit under attack, unit lost).** Poll-based observe
  works for planning but LLMs will miss reactive triggers. Add if
  reactive behavior becomes important.
- **Multi-game concurrency model.** Thread-per-room vs
  pool-of-workers. Decide when task #14 lands.

## Code layout

Server-side (all under `server/`):

- `main.cpp` — server binary. Loads map, binds sync + WS ports,
  spawns virtual clients, drives the sim tick loop, drains command
  and observe queues per tick.
- `auth.h` — user_registry: loads users.json, hashes API keys with
  SHA-256, `verify(key) → user_t*`.
- `sha256.h` — public-domain SHA-256 used by auth.
- `keygen.cpp` — `openbw_keygen` CLI, emits a users.json entry with a
  fresh 32-byte random key.
- `auth_test.cpp` — `openbw_auth_test` binary, sanity checks
  (SHA-256 NIST vectors, registry lookup, dup rejection).
- `command_queue.h` — per-slot mutex-guarded deque, drained in slot
  order each tick.
- `agent_protocol.h` — JSON → BW action bytes encoder. Verbs: move,
  attack, stop, train, build.
- `ws_server.h` — hand-rolled WebSocket server on asio 1.10.
  Handles the HTTP upgrade, `?key=<api_key>` auth, masked text frame
  parse/emit, dispatches `type:"cmd"` and `type:"observe"`.
- `observe_request.h` — per-slot observation request queue.
- `observation.h` — sim-thread snapshot serializer; respects fog of
  war for enemies, breaks out neutrals.

Sync layer (single header, shared with `ui/`):

- `sync.h` — extended with:
  - `id_auth`, `id_assign_perspective`, `id_catchup_data`,
    `id_agent_action` message types.
  - `sync_state::auth_check`, `perspective_for`, `catchup_provider`
    callbacks, `outgoing_api_key`, `viewing_slot`,
    `initial_rand_state`, `virtual_clients_by_slot`.
  - `handle_catchup`, `start_game_local`, `broadcast_agent_action`.
  - `all_clients_in_sync` excludes observers (player_slot == -1) and
    virtual clients (h == nullptr) from the lag gate.

Observer client:

- `ui/observer.cpp` — SDL2 spectator. Connects before creating the
  window (avoids macOS WindowServer stalls), waits briefly for the
  handshake, then enters the sim loop.
- `ui/ui.h` — added `viewing_slot` + fog rendering
  (`draw_fog` and minimap fog use `dark_pcx` row 7).

Test harness:

- `test_resources/users.json` — 8 pre-generated player keys (alice
  … henry, slots 0..7). Gitignored.
- `test_resources/test_guidance.md` — copy-paste playbook.

## References

- `sync.h` lines around `execute_scheduled_actions`,
  `all_clients_in_sync`, and `schedule_action` — the core sync
  loop the whole observer + agent stack builds on.
- `mini-openbwapi/` — original inspiration for the command /
  observation surface, though our WS protocol is JSON-first.
- `replay_saver.h` / `replay.h` — action-log serialization reused
  for observer late-join.
