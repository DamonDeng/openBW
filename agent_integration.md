# Agent Integration Guide

How to build an agent that plays StarCraft: Brood War through the openBW
server. This is the workshop-facing spec: the server exposes a minimal
raw-JSON WebSocket API, and it's the attendee's job to write a language
SDK (Python or otherwise) on top of it.

The C++ server intentionally stays lean. Everything LLM-friendly —
name lookups, prompt shaping, event streams, retry policies — belongs in
your agent code, not in the server.

## What the server gives you

- **Deterministic sim**: 24 frames/sec, authoritative on the server,
  identical Brood War rules openBW is famous for.
- **Per-slot identity**: your API key maps to one player slot (0-7).
- **Two message flavors over one WebSocket**:
  - `observe` → snapshot of what your slot can see.
  - `cmd` → order units around (move, attack, stop, train, build).
- **Machine-readable enum tables** (`agent_reference/*.json`) so you
  don't have to transcribe 228 unit types by hand.

That's it. No RPC framework, no session management, no rate limiting,
no built-in prompt formatting. Add whatever you need in your SDK.

---

## Connecting

```
ws://<server_host>:6113/agent?key=<your_api_key>
```

Two things about the URL:
- **Query-string auth**: `?key=<api_key>` is required. The server verifies
  against `users.json` before completing the WebSocket handshake. Bad
  key → HTTP 401 and the socket closes.
- **Path `/agent`**: currently ignored. Future control-plane endpoints
  may use other paths.

On successful upgrade, the server sends **one** welcome frame, then
waits for you.

```json
{"type": "welcome", "slot": 0, "current_frame": 42}
```

`slot` is your assigned player slot (from users.json). `current_frame`
is the server's sim frame at the moment you connected. Frames advance
at 24 Hz.

## Message schema

Every message is a single text frame containing one JSON object.
Every object has a `type` field.

### Client → server

Request an observation:

```json
{
  "type": "observe",
  "id": "<opaque request id>",
  "targets": ["units", "enemies", "resources", "map_info"]
}
```

- `id` is echoed back in the response. Use it to correlate concurrent
  requests. Any string.
- `targets` is optional. Default (missing / empty array) =
  `["units", "enemies", "resources"]`. Pass `["all"]` to include
  everything including `map_info` (usually you fetch that once).

Send a command:

```json
{
  "type": "cmd",
  "id": "<opaque request id>",
  "cmd": { "verb": "<move|attack|stop|train|build>", ... }
}
```

Command payloads vary by verb — see [Commands](#commands) below.

### Server → client

```json
// One-shot on connect.
{"type": "welcome",     "slot": 0, "current_frame": 42}

// Reply to a "cmd" request.
{"type": "ack",         "id": "m1", "queued_at_frame": 43}

// Reply to any request that didn't validate.
{"type": "error",       "id": "m1", "message": "..."}

// Reply to an "observe" request. See "Observation format" below.
{"type": "observation", "id": "o1", "slot": 0, "current_frame": 123, ...}
```

**Important**: `ack` means "we queued your command". It does NOT mean
the unit obeyed. Sometimes a command is silently dropped inside the
sim (unit dead, insufficient resources, invalid target). To confirm
effect, `observe()` again a few frames later and check the unit's
`order` and `x`,`y` fields.

---

## Commands

All five verbs take a `unit` field: the id of the unit you're
commanding. **Get unit ids from an `observe()` response — they're not
just indices** (see [Unit ids](#unit-ids)).

The server auto-issues a "select unit(s)" action before every verb, so
you don't need to select first as you would when playing manually.

### Move

Move a unit to a map coordinate.

```json
{"verb": "move", "unit": 3684, "x": 2000, "y": 2000, "queue": false}
```

- `x`, `y` are pixel positions on the map (see
  [Coordinate system](#coordinate-system)).
- `queue` optional. `true` = append after current orders, `false` /
  omitted = replace.

### Attack

Attack a specific unit, or attack-move to a position.

```json
// Attack a specific target.
{"verb": "attack", "unit": 3684, "target_unit": 3699, "x": 0, "y": 0}

// Attack-move (no target = go there and attack anything on the way).
{"verb": "attack", "unit": 3684, "target_unit": 0,    "x": 2000, "y": 2000}
```

- `target_unit`: unit id of the target, or `0` for attack-move.
- `x`,`y`: only used for attack-move. Ignored if `target_unit != 0`.

### Gather

Send a worker to mine minerals or harvest gas.

```json
{"verb": "gather", "unit": 3684, "target_unit": 3721}
```

- `unit`: id of the worker (SCV / Drone / Probe).
- `target_unit`: id of the mineral field or vespene geyser (from the
  `neutrals` list in an observation).

**Note**: attacking a mineral field with the `attack` verb won't
start a gather cycle -- retail BW does that translation on the game
client, not in the sim. Use `gather` for mining.

### Stop

Cancel current orders.

```json
{"verb": "stop", "unit": 3684, "queue": false}
```

### Train

Have a building (or larva) train a new unit.

```json
{"verb": "train", "unit": 3720, "unit_type": 7}
```

- `unit`: id of the training building (Command Center, Barracks, Larva,
  Nexus, Gateway, etc).
- `unit_type`: integer id of the unit to train. See
  `agent_reference/unit_types.json` for the full mapping.

### Build

Have a worker place a building.

```json
{"verb": "build", "unit": 3684, "unit_type": 106, "tile_x": 24, "tile_y": 30}
```

- `unit`: id of the worker (SCV/Drone/Probe).
- `unit_type`: integer id of the building to place.
- `tile_x`, `tile_y`: position in **tile** units (1 tile = 32 pixels).
  See [Coordinate system](#coordinate-system).

The order (Terran `PlaceBuilding` vs Protoss `PlaceProtossBuilding`)
is auto-selected from the building's `unit_type` id. Zerg buildings
morph from a Larva -- use the `train` verb, not `build`, for those.

To find a **valid** tile (not blocked by minerals, unbuildable
terrain, or unexplored fog) use the [`find_placement`](#find_placement-query)
query first.

### find_placement (query)

Not a command -- a read-only query. Asks the server "where can I put
a building of this type?" and gets back a list of valid tiles.

```json
{"type": "find_placement", "id": "fp1",
 "unit_type": 156,               // Protoss_Pylon
 "worker_unit": 3684,            // optional; anchors search around this unit
 "center_x": 3800, "center_y": 2400,   // optional; alternative anchor
 "radius_tiles": 12,             // optional; default 12
 "max_results": 8}               // optional; default 24
```

Response:

```json
{"type": "placement_result", "id": "fp1",
 "unit_type": 156, "tile_size_x": 2, "tile_size_y": 2,
 "spots": [
   {"tile_x": 119, "tile_y": 76, "center_x": 3840, "center_y": 2464},
   {"tile_x": 120, "tile_y": 76, "center_x": 3872, "center_y": 2464},
   ...
 ]}
```

Spots are ordered nearest-first. Empty `spots` means nothing valid
in the search radius. Feed `spot["tile_x"]`, `spot["tile_y"]` back
into a `build` command to place the building.

The server uses the sim's `can_place_building` internally, so if
`spots` contains a tile, the subsequent `build` command will
succeed (assuming enough minerals + a live worker).

---

## Observation format

Response to `{"type":"observe", ...}`:

```json
{
  "type": "observation",
  "id": "o1",
  "slot": 0,
  "current_frame": 123,
  "resources": {
    "minerals": 50,
    "gas": 0,
    "supply_used": 8,
    "supply_max": 20,
    "minerals_gathered": 50,
    "gas_gathered": 0
  },
  "units": [
    {
      "unit_id": 3684,
      "type": 7,
      "x": 3832, "y": 2440,
      "hp": 60, "hp_max": 60,
      "energy": 0,
      "order": 3,
      "completed": true
    }
    // ...
  ],
  "enemies":  [ /* only visible ones */ ],
  "neutrals": [ /* mineral fields, geysers, etc. */ ],
  "map_info": { "width": 4096, "height": 4096,
                "tile_width": 128, "tile_height": 128,
                "tileset": 0 }
}
```

### Per-unit fields

Always present:

- `unit_id` — 16-bit unit id (see [Unit ids](#unit-ids)).
- `type` — integer from `agent_reference/unit_types.json`.
- `x`, `y` — pixel position.
- `hp`, `hp_max` — integer hit points.
- `order` — integer from `agent_reference/orders.json`. What the unit
  is currently doing.

Present when applicable:

- `shields`, `shields_max` — Protoss units.
- `energy` — units with mana (High Templar, Ghost, Battlecruiser, etc).
- `completed` — true unless still under construction.
- `flying` — true for air units currently airborne.
- `burrowed` — true for burrowed Zerg.
- `cloaked` — true for cloaked units.
- `building` — true for buildings.
- `owner` — only on enemies/neutrals; index into the players array.

### Enemies vs neutrals

- `enemies`: units owned by another player (slots 0-7) that your slot
  can currently see (fog of war respected).
- `neutrals`: mineral fields, gas geysers, and other neutral map
  objects (owner 11). Always shown if in visual range.

If you see something in `neutrals` at position (x,y), that spot is
worth mining once you have a worker there.

---

## Coordinate system

- **Pixel coordinates** (`x`, `y` in observations, `x`, `y` in
  move/attack): fine-grained world positions, top-left origin.
- **Tile coordinates** (`tile_x`, `tile_y` in build): each tile is
  **32 pixels** on a side. Convert: `tile_x = pixel_x / 32`.
- Map bounds: `map_info.width` and `map_info.height` in pixels;
  `map_info.tile_width` and `tile_height` in tiles. A typical
  128×128-tile map = 4096×4096 pixels.

Attack-move to a location: use pixel coordinates.
Place a building: use tile coordinates.

---

## Unit ids

`unit_id` is a **16-bit value that packs both an array index and a
generation counter**. This means:

- `unit_id = 0` is never a valid unit — treat it as "no target" (for
  `target_unit` in attack).
- Small integers like 1, 2, 3, ... **are not** unit ids you can guess.
  They're internal placeholder slots.
- Real ids look like 3684, 3689, 4712, etc. Get them from an
  `observe()` response.
- When a unit dies and its slot gets recycled, the new occupant gets a
  **different** unit_id (the generation bits change). Never cache ids
  for long.

Rule of thumb: your agent's loop is
`observe() → decide → send cmds referencing units from the observe →
observe() again`. Never send a command with a unit id from more than a
few seconds ago.

---

## Enum reference tables

Two JSON files ship under `agent_reference/`:

- **`unit_types.json`** — 229 entries, keyed by string integer id.
  Values are enum names from `bwenums.h`, e.g. `"7": "Terran_SCV"`,
  `"41": "Zerg_Drone"`, `"64": "Protoss_Probe"`,
  `"106": "Terran_Command_Center"`, `"131": "Zerg_Hatchery"`,
  `"154": "Protoss_Nexus"`.

- **`orders.json`** — 190 entries with the same shape. The ones an
  agent commonly sees on observed units:
  - `"0": "Die"`, `"1": "Stop"`, `"2": "Guard"`, `"3": "PlayerGuard"`
    (default idle for owned units).
  - `"6": "Move"`, `"10": "AttackUnit"`, `"14": "AttackMove"`.
  - `"23": "Nothing"` (buildings that aren't producing).
  - `"25": "DroneStartBuild"`, `"26": "DroneBuild"`,
    `"30": "PlaceBuilding"`, `"33": "ConstructingBuilding"` (workers
    building).
  - `"34": "Repair"` (SCV repair).
  - `"79": "Harvest1"`, `"80": "Harvest2"`, `"83": "HarvestGas"`,
    `"87": "MiningMinerals"` (workers gathering).

Load once at agent startup:

```python
import json
UNIT_TYPES = {int(k): v for k, v in json.load(open("agent_reference/unit_types.json")).items()}
ORDERS     = {int(k): v for k, v in json.load(open("agent_reference/orders.json")).items()}
```

Or hand-transcribe just the ~20 units and 10 orders your agent
actually needs. The full tables are big; most agents use a small
subset.

The tables were extracted from `bwenums.h` with
`tools/extract_enums.py`. Re-run it if openBW's enums ever change.

---

## Frame timing

- **24 frames per second**, wall-clock.
- Commands you `ack` at `queued_at_frame: F` land on frame `F`
  (server assigns this deterministically as `current_frame + 1`).
- Your agent's decision latency **doesn't slow the sim**. If your LLM
  takes 500 ms to respond, that's 12 frames the sim advances without
  you. Units keep executing whatever their last order was (mining,
  attacking, patrolling), so idle time isn't wasted — just uncommanded.
- If you need finer-grained reactivity, `observe()` more often. If you
  want smoother units, `queue: true` a couple of orders ahead.

---

## Fog of war

Your `enemies` list only contains units your slot can currently see —
i.e., ones inside vision range of your own units. Once your scout
leaves, those enemies drop out of subsequent observations even if
they're still alive on the server.

Tips:

- Track last-known positions in your agent's memory; the server won't.
- `neutrals` (mineral fields, gas geysers) are always visible when in
  sight range. They're static — no need to re-observe locations.
- The observer's SDL2 window (`openbw_observer`) can be launched with
  the same API key so a human can spectate the exact view the agent
  sees. Handy for debugging.

---

## LLM-as-agent pattern

The natural loop for an LLM-powered agent:

```
1. observe()
2. build a prompt from the observation
3. LLM decides which tool call to make (move/attack/train/etc)
4. send cmd
5. wait N frames (or event; simplest: sleep 0.5-2 seconds)
6. goto 1
```

### Keeping prompts lean

A mid-game observation for one player is ~20-100 units. Serialized
directly, that's ~1-5k tokens. Some strategies:

- **Summarize by group**: instead of listing every SCV, emit "12 SCVs
  at base (min HP 60)". Your agent only needs to fine-target a few.
- **Filter by relevance**: units at full HP doing their job don't need
  to be in the prompt. Include only under-attack units, idle
  workers, incomplete buildings, and enemies.
- **Cache the map**: fetch `map_info` once, hold it locally. Don't
  re-fetch every observe.
- **Batch observations**: instead of "observe → think → 1 cmd →
  observe again", let the LLM plan a sequence of commands per
  observation. Frame timing is forgiving.

### Tool schema for LLM function calling

Expose our five verbs as function tools. Rough shape:

```
move(unit_id: int, x: int, y: int, queue: bool = false)
attack(unit_id: int, target_unit_id: int = 0, x: int = 0, y: int = 0)
stop(unit_id: int)
train(unit_id: int, unit_type: int)     # or unit_type_name: str, mapped locally
build(unit_id: int, unit_type: int, tile_x: int, tile_y: int)
```

Wrap each in Python and translate to the JSON wire format. Most LLM
providers accept a JSON schema per tool.

### Names vs ids

Server always uses **integer ids**. In your SDK, feel free to accept
names in the prompt-facing API and map them via `UNIT_TYPES`:

```python
def train(unit_id, name):
    return {"verb":"train", "unit": unit_id, "unit_type": name_to_id(name)}
```

This is the kind of thing that lives in your SDK, not on the server.

---

## Minimal working agent (Python)

```python
import asyncio, json, sys
import websockets

async def run(key: str, host: str = "127.0.0.1", port: int = 6113):
    async with websockets.connect(f"ws://{host}:{port}/agent?key={key}") as ws:
        welcome = json.loads(await ws.recv())
        print("connected as slot", welcome["slot"])

        # One-time: fetch map dimensions.
        await ws.send(json.dumps({"type":"observe","id":"map","targets":["map_info"]}))
        map_info = json.loads(await ws.recv())["map_info"]
        center_x = map_info["width"] // 2
        center_y = map_info["height"] // 2

        while True:
            # 1. observe
            await ws.send(json.dumps({"type":"observe","id":"o"}))
            obs = json.loads(await ws.recv())
            print(f"frame={obs['current_frame']} units={len(obs['units'])} "
                  f"minerals={obs['resources']['minerals']}")

            # 2. decide: pick any idle worker, send it exploring
            workers = [u for u in obs["units"]
                       if u["type"] in (7, 41, 64)   # SCV, Drone, Probe
                       and u["order"] in (2, 3)]    # Guard / PlayerGuard = idle
            if workers:
                w = workers[0]
                await ws.send(json.dumps({
                    "type":"cmd", "id":"m",
                    "cmd":{"verb":"move","unit":w["unit_id"],
                           "x":center_x, "y":center_y}
                }))
                ack = json.loads(await ws.recv())
                print(f"moved unit {w['unit_id']} → applied at frame {ack['queued_at_frame']}")

            await asyncio.sleep(1.0)

asyncio.run(run(sys.argv[1]))
```

Save as `simple_agent.py`. Run with your API key:

```bash
python3 simple_agent.py sk-LYvXIzRDaDEe8GTlPgyf8eMxAyamUJt_Ig2413DbEjw
```

Watch what your agent does by launching an `openbw_observer` window
with the same key.

---

## Known limitations

Things the workshop version doesn't (yet) do — heads up so your SDK
can handle them gracefully.

1. **No error replies for rejected commands.** If you send `train` with
   insufficient minerals, or `attack` a dead unit, the command silently
   drops inside the sim. `ack` still comes back. Your only tell is
   that the next `observe()` shows nothing changed.

2. **No push events.** No "unit died", "under attack", "nuke detected"
   notifications. Poll via `observe()`. Compare against previous
   observations in your SDK if you need change detection.

3. **Unit `type_name` is not on the wire.** You get integer `type` and
   map it locally via `unit_types.json`.

4. **Race and starting-unit initialization has quirks.** Some maps
   spawn different starting units than you'd expect from the assigned
   race. Always `observe()` to see what you actually have — don't
   assume "I'm playing Terran, so I have an SCV at unit_id X".

5. **No `queue` for train/build.** The `queue` field is only respected
   on move / attack / stop right now.

6. **Only one game per server process.** No matchmaking, no rooms.
   Attendees each get their own server if running independent games.

---

## Debugging

- **Watch what your agent is doing**: launch `openbw_observer` with
  your API key. You'll see the agent's slot perspective in a live
  window.
- **Server log**: the server prints `[srv]` and `[ws]` lines. Key
  lines to watch for:
  - `auth OK: alias=<you> slot=<n>` — your WS or observer just
    authenticated.
  - `catchup bundle: frame=N action_bytes=B` — a late-joining
    observer just requested history.
  - `frame=N observers=X virtual-agents=Y pending-cmds=Z` — heartbeat
    each second. `pending-cmds` should return to 0 quickly; if it
    stays high, the sim is falling behind.
- **Reproducing a bug**: capture `seed`, the map, and the exact
  sequence of commands. The sim is deterministic — replaying the same
  action stream against the same seed produces the same state.

---

## Where to look in the code

Not required for building agents, but if you want to understand the
server internals:

- Server main loop: `server/main.cpp`
- WebSocket layer: `server/ws_server.h`
- Command encoding: `server/agent_protocol.h`
- Observation serialization: `server/observation.h`
- Sync protocol (extended for auth/perspective/catchup/agent_action):
  `sync.h`
- Enum definitions: `bwenums.h` (source of truth for the JSON tables)

Overall architecture and design rationale: `agent_readme.md`.
