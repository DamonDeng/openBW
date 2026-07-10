# python_agent — reference Python agent + client

Sample Python implementation of an agent that plays through the openBW
server's WebSocket API. Serves double duty as **workshop starter code**
(fork it and add strategy) and our **automated smoke test** (regression
check after server changes).

## Layout

```
python_agent/
├── client.py                # thin async WebSocket client (one method per API call)
├── enums.py                 # unit-type / order name<->id lookups
├── helpers.py               # shared utilities: find workers, nearest, race, etc.
├── agents/
│   ├── p_agent_v4.py        # ⭐ Protoss: v3 + Carrier/Reaver fighter refill
│   ├── p_agent_v3.py        # Protoss: v2 + scouting + wider spread + upgrades
│   ├── p_agent_v2.py        # Protoss: coverage-oriented (one of every building/unit)
│   ├── p_agent_v1.py        # Protoss: early integrated agent (superseded by v2+)
│   ├── t_agent_v4.py        # ⭐ Terran: v3 + SCV repair (mirrors p_agent_v4)
│   ├── ai_debug_agent.py    # minimal bisection tool for sync-divergence hunts
│   ├── random_walk.py       # move idle workers to random points
│   ├── miner.py             # gather minerals; top up gas workers per refinery
│   ├── trainer.py           # train workers + combat units when producers ready
│   ├── builder.py           # walk a race build order: supply→gas→producer→tech
│   └── attacker.py          # attack-move combat units toward enemy corner
├── smoke_test.py            # spawns server + runs scripted scenarios
└── README.md                # this file
```

The full-race agents follow a `<race>_agent_v<n>.py` naming scheme:
`p_` for Protoss, `t_` for Terran (in progress), `z_` for Zerg
(not yet implemented). Version numbers escalate capability: v1 is a
single closed loop, v2 adds coverage, v3 adds scouting/upgrades, v4
adds race-specific "maintenance" features (Carrier/Reaver fighter
refill for Protoss, SCV repair for Terran, etc).

## Which agent to run

**Protoss agents:**

- **For the fullest coverage: `p_agent_v4`.** Everything v3 does
  plus the `train_fighter` verb: maintains a full complement of
  Interceptors in each Carrier and Scarabs in each Reaver using
  the observation's `fighter_count` / `fighter_max` fields.
- **For a slightly simpler agent: `p_agent_v3`.** Everything v2 does
  (one of every building + one of every unit) plus 2-3 probes on
  radial-from-home scouting patrol, wider building distribution via
  rotating anchor strategies, and the `research`/`upgrade` verbs
  exercised via a small per-race upgrade catalog.
- **For a simpler agent: `p_agent_v2`.** Just build/train coverage.
  No scouts, no upgrades. Good for isolating specific verb bugs.
- **For historical / integrated single-loop agent: `p_agent_v1`.** One process, one connection,
  one decision loop, one intent store. It verifies each build/gas
  assignment by observing outcomes (was a matching building placed?
  is `resources.gas` actually rising?) and retries when the sim
  silently drops a command. Superseded by v2/v3 for coverage
  testing, but a good compact reference for the closed-loop pattern.
**Terran agents:**

- **For the fullest coverage: `t_agent_v4`.** Terran counterpart of
  p_agent_v4. Everything the Protoss version does (scouting,
  expansion, catalog-driven build/train/upgrade) but adapted for
  Terran's Barracks/Factory/Starport tech tree. Flagship feature:
  **SCV repair** via the new `repair` verb. For every own damaged
  mechanical unit (vehicle, air, or building), pulls the nearest
  idle SCV to repair it -- keeps late-game investments (Siege Tanks,
  Battlecruisers, Science Vessels) alive through firefights
  instead of just watching them die. Addons (Machine Shop, Comsat,
  Control Tower) are deferred to a later revision.

**Launching a Terran run:** BOTH the server AND the observer need
`--race N=terran` args -- observer's map load runs before it gets
race info from server, so if you skip the flag on the observer
you'll see Protoss starting units even though the server sim is
Terran (agent still plays correctly, just visually wrong).

**Zerg agent (`z_agent_v*`):** not yet implemented.

- **For learning / workshop demos: the split agents** (miner, trainer,
  builder, attacker). Each is <100 lines and focuses on one verb.
  They work fine in isolation and are easy to fork, but running all
  four together on the same slot has known open-loop issues — see
  `p_agent_v1.py`'s docstring for the failure modes it fixes.

The five split agents cover the core BW verbs and, together, form a
minimal opening:

- **random_walk** — move + observe loop
- **miner** — gather; prioritizes gas (~3 workers per completed
  refinery) then sends the rest to minerals
- **trainer** — train workers up to `--worker-cap` (default 16), then
  train combat units from any completed producer building; Zerg
  alternates on the shared Larva
- **builder** — walks a race-specific build order via
  `find_placement`: supply → gas → producer → tech (Protoss:
  Pylon → Assimilator → Gateway → Cybernetics Core; Terran:
  Supply Depot → Refinery → Barracks; Zerg partially wired, see
  builder's docstring)
- **attacker** — attack + attack-move; combines observe of enemies +
  target selection; won't chatter (leaves already-moving units alone)

Combine them freely on one slot: `miner + trainer + builder + attacker`
gives an end-to-end opening that mines, expands to gas, techs up,
trains a combat unit, and sends it toward the enemy base.

**Race**: agents infer the race from the first observed worker / main
structure. To pick a race explicitly, use the server's `--race`
flag (see `../test_resources/test_guidance.md`).

Zero dependencies beyond `websockets`. Python 3.9+ (uses dataclasses,
`asyncio.run`, and type-hint syntax like `dict[int, str]`).

## Setup

```bash
# Server must be built (see ../agent_readme.md for build instructions).
# One-time Python setup:
pip3 install --break-system-packages websockets
```

## Running the sample agents

Both agents take an API key as the first positional arg. Get keys from
`test_resources/users.json`.

Start the server in one terminal:

```bash
./build_srv/server/openbw_server \
  --map "original_resources/(2)Bottleneck.scm" \
  --data-path original_resources \
  --users test_resources/users.json

# To force a specific race per slot (map's default is used otherwise):
#   --race 0=terran --race 1=zerg
```

Then in another terminal:

```bash
# Recommended: latest integrated agent (Protoss):
python3 -m python_agent.agents.p_agent_v4 KEY                    # Protoss: v3 + fighter refill
python3 -m python_agent.agents.p_agent_v3 KEY                    # Protoss: v2 + scouting/upgrades
python3 -m python_agent.agents.p_agent_v2 KEY                    # Protoss: coverage-only
python3 -m python_agent.agents.p_agent_v1 KEY                    # Protoss: historical
python3 -m python_agent.agents.t_agent_v4 KEY                    # Terran: v3 + SCV repair
                                                                 #   Needs matching --race N=terran on both
                                                                 #   server and observer.

# Or run the individual demos:
python3 -m python_agent.agents.random_walk KEY
python3 -m python_agent.agents.miner       KEY                    # --gas-workers 3
python3 -m python_agent.agents.trainer     KEY                    # --worker-cap 16
python3 -m python_agent.agents.builder     KEY                    # --supply-gap 3
python3 -m python_agent.agents.attacker    KEY

# Where KEY = one of the api_key values in test_resources/users.json.
# Alice's key (slot 0):
#   sk-LYvXIzRDaDEe8GTlPgyf8eMxAyamUJt_Ig2413DbEjw
# Bob's (slot 1):
#   sk-anYTfuY-QL9szAzIlvtv44RxpgJlJPC1ocqIA26qpf0
```

Each agent supports `--help` for its flags. The typical tuning is
`--worker-cap` (higher = more mining, later combat) and
`--gas-workers` (default 3 is BW's saturation).

Multiple agents can run for the same slot — miner + trainer together
gives you a functional economy on alice's side. Just launch them in
separate terminals with the same key.

Launch `openbw_observer` with the same key to watch what your agent
does.

## Running the smoke test

Spawns the server itself, exercises the API, asserts, tears down:

```bash
python3 -m python_agent.smoke_test
```

Exits 0 on success, non-zero on failure (server output dumped on
failure to help you debug). Run this after any server-side change.

## Layers, and which to use

**`python_agent.client.Client`** is the low-level layer. One method per
wire message type. Every method returns the raw JSON dict from the
server. Use this directly if you want maximum control:

```python
from python_agent.client import Client

async with Client(api_key="sk-...") as c:
    print(c.welcome)             # -> Welcome(slot=0, current_frame=42)
    obs = await c.observe()      # -> raw observation dict
    ack = await c.move(unit_id=obs["units"][0]["unit_id"], x=1000, y=1000)
```

**`python_agent.enums`** gives you name<->id lookups so your code
doesn't have magic numbers everywhere:

```python
from python_agent.enums import unit_type_id, unit_type_name, order_name
scv = unit_type_id("Terran_SCV")     # 7
unit_type_name(64)                   # "Protoss_Probe"
order_name(6)                        # "Move"
```

Higher-level things (find idle workers, pick nearest mineral, LLM tool
schemas) live in your agent code — this package intentionally stops
here. See `agents/random_walk.py` and `agents/miner.py` for how to
compose the two layers into a working loop.

## Writing your own agent

Start from `agents/random_walk.py`. Common changes:

1. Replace the "pick a random worker" logic with your strategy.
2. Change the `interval_sec` if you want faster/slower reaction.
3. Import from `python_agent.enums` to keep numbers out of your code.

For LLM-driven agents, see the "LLM-as-agent pattern" section in
[`../agent_integration.md`](../agent_integration.md). The typical
shape:

```python
while True:
    obs = await c.observe(targets=["units", "enemies", "resources"])
    prompt = build_prompt(obs)               # your prompt shaping
    tool_call = await llm.chat(prompt)       # your LLM provider
    verb, args = parse_tool_call(tool_call)  # your parsing
    await getattr(c, verb)(**args)
    await asyncio.sleep(1.0)
```

## Gotchas

Most of these are documented in more detail in `agent_integration.md`.

- **`unit_id` includes a generation counter.** Don't cache ids across
  observations if a unit might have died.
- **Successful `ack` != successful command.** If the sim rejects a
  command (dead target, no minerals, etc), you'll get an ack but the
  observation won't reflect any change. Re-observe to confirm.
- **`order` values are integers.** Map via `enums.ORDERS` to make
  logging + debugging readable.
- **The server is one game per process.** Multiple attendees running
  their own experiments should each launch their own server.
