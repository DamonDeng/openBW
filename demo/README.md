# demo/

One-command local demo of the whole simsc stack: one game, two
agents, three observer windows.

## Usage

Run from the repo root:

```bash
python3 demo/run_demo.py
```

That's it. Six processes come up:

- `openbw_server` on ports 6113 (agent WS) + 6114 (observer WS)
- two agents, latest per resolved race, one per slot
- three `simsc_app` windows:
  1. **slot 0** — Alice's per-slot perspective (fog + HUD readouts)
  2. **slot 1** — Bob's per-slot perspective (fog + HUD readouts)
  3. **admin** — no fog, no per-slot readouts (spectator)

Ctrl-C once and everything tears down cleanly. A second Ctrl-C
force-kills.

## Options

| flag            | default                                | notes                             |
|-----------------|----------------------------------------|-----------------------------------|
| `--map PATH`    | `original_resources/(2)Bottleneck.scm` | Any 2-player .scm/.scx           |
| `--race N=RACE` | random for each unspecified slot       | `N` is 0 or 1; `RACE` is `zerg`/`terran`/`protoss`. Repeat once per slot |
| `--game-speed`  | `10` (turbosuper)                      | BW name or int ms/frame          |
| `--agent-port`  | 6113                                   |                                   |
| `--obs-port`    | 6114                                   |                                   |
| `--log-dir`     | `demo/logs`                            | per-process stderr/stdout sinks   |

Examples:

```bash
# Random races on the default map:
python3 demo/run_demo.py

# Explicit races:
python3 demo/run_demo.py --race 0=protoss --race 1=zerg

# Different map, retail-fastest tick rate:
python3 demo/run_demo.py \
    --map 'original_resources/(2)Volcanis.scm' \
    --game-speed fastest
```

## Latest agents (bump when new versions ship)

| race    | module                            |
|---------|-----------------------------------|
| zerg    | `python_agent.agents.z_agent_v5`  |
| terran  | `python_agent.agents.t_agent_v6_7`|
| protoss | `python_agent.agents.p_agent_v4`  |

Edit `AGENT_MODULES` in `run_demo.py` to change.

## API keys

The launcher uses hard-coded demo keys:

- `sk-alice` → slot 0 player
- `sk-bob` → slot 1 player
- `sk-admin` → admin (no fog)

**Demo only.** Not for production use.

## Logs

Each process writes to `demo/logs/`:

```
server.log        agent_slot0.log     simsc_slot0.log
                  agent_slot1.log     simsc_slot1.log
                                      simsc_admin.log
```

`demo/logs/` is gitignored.

## Requirements

The launcher assumes both binaries are built:

```bash
cmake --build build_srv --target openbw_server
cmake --build build_qt  --target simsc_app
```

Preflight fails fast with actionable messages if either is missing
or if ports 6113/6114 are already in use (usually a stale server —
`pkill -f openbw_server` and retry).
