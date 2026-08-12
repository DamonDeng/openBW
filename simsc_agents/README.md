# simsc_agents — one launchable per file

This directory holds the agents the Qt lobby (`simsc_desktop`) offers
in its "Attach agent" picker. Every entry is a **single executable
file** that, when run, plays a game on behalf of one slot.

## The launch contract

The lobby invokes each entry with exactly this argv, nothing else:

```
<wrapper-file> --url <base-ws-url> --api-key <key> --race <r>
```

- `--url` is the WebSocket URL WITHOUT the query string. Your agent
  appends `?key=<api-key>` itself when opening the connection.
- `--api-key` is the API key for the slot the agent is playing on.
- `--race` is one of `zerg`, `terran`, `protoss`. Single-race agents
  may ignore this flag but MUST accept it without error.

Random race + slot resolution happens BEFORE the picker is even
clickable — the openBW server rejects `random` at CLI parse. See
`python_agent/agent_cli.py` for the shared helper Python agents use
to parse this contract.

## Wrapper convention

Each entry is a small script whose only job is to invoke the real
agent implementation with the same argv. This is deliberate: keeps
the "one executable per launchable" rule tidy and lets the actual
implementation live wherever makes sense for its language / build
system (source tree, virtualenv, Docker image, compiled binary,
etc.).

### For Python agents in this repo

```bash
#!/usr/bin/env bash
# simsc_agents/z_agent_v5
cd "$(dirname "$0")/.." && exec python3 -m python_agent.agents.z_agent_v5 "$@"
```

The `cd` puts the repo root on Python's `sys.path` so `from
python_agent...` resolves. The `exec` replaces the shell process so
signals from the Qt lobby (SIGTERM on detach, SIGKILL at shutdown)
reach Python directly.

### For agents in another repo / language

The wrapper is just a shell script. Anything is fine as long as the
end result honours the launch contract:

```bash
#!/usr/bin/env bash
# simsc_agents/my_rust_bot
exec /path/to/my_rust_bot_binary "$@"
```

```bash
#!/usr/bin/env bash
# simsc_agents/my_docker_bot
exec docker run --rm --network host my-org/my-bot:latest "$@"
```

## Filename = display name

The lobby picker shows each wrapper's filename (minus extension) as
the display name. Sort order is case-insensitive ASCII. Hidden files
(leading dot) are skipped.

## Attendee workflow

To make your agent launchable from the Qt lobby:

1. Drop a wrapper file in this directory (or wherever your Settings
   "Agents directory" points).
2. `chmod +x <wrapper>`.
3. Restart the lobby (or reopen the Attach dialog — it rescans on
   each open).

That's it. Nothing else about your agent code needs to know about
the lobby.
