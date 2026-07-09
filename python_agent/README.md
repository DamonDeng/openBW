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
│   ├── random_walk.py       # move idle workers to random points
│   ├── miner.py             # send idle workers to gather nearest minerals
│   ├── trainer.py           # keep the main producer training workers
│   ├── builder.py           # place a supply-cap building near main (DEMO)
│   └── attacker.py          # attack-move combat units toward enemy corner
├── smoke_test.py            # spawns server + runs scripted scenarios
└── README.md                # this file
```

The five agents cover the core BW verbs:

- **random_walk** — move + observe loop
- **miner** — gather (workers actually mine, minerals accumulate)
- **trainer** — train (producers build new units, resources consumed)
- **builder** — build (issues the verb; placement math is a learning
  exercise -- see the file's docstring)
- **attacker** — attack + attack-move; combines observe of enemies +
  target selection

Combine them freely: `miner + trainer` gives a functional economy on
one slot. `attacker` needs combat units so pair it with a trainer +
tech that produces them (attendee work).

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
```

Then in another terminal:

```bash
# Pick one:
python3 -m python_agent.agents.random_walk KEY
python3 -m python_agent.agents.miner       KEY
python3 -m python_agent.agents.trainer     KEY
python3 -m python_agent.agents.builder     KEY
python3 -m python_agent.agents.attacker    KEY

# Where KEY = one of the api_key values in test_resources/users.json.
# Alice's key (slot 0):
#   sk-LYvXIzRDaDEe8GTlPgyf8eMxAyamUJt_Ig2413DbEjw
# Bob's (slot 1):
#   sk-anYTfuY-QL9szAzIlvtv44RxpgJlJPC1ocqIA26qpf0
```

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
