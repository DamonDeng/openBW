# Agent command status codes

Every accepted agent command generates exactly one `result` message
sent back to the client that issued it. This document is the
canonical mapping between the numeric `status` field on the wire and
what it means. Python agents that use the `python_agent.client`
package get symbolic access via `python_agent.status.Status`.

## Message shapes

Client → server (unchanged):
```json
{"type": "cmd", "id": "r42", "cmd": {"verb": "train", "unit": 3684, "unit_type": 7}}
```

Server → client, in order:

1. Enqueue-time (immediate):
```json
{"type": "ack", "id": "r42", "queued_at_frame": 218}
```
   Or if the server rejects before enqueue (bad JSON, unknown slot):
```json
{"type": "error", "id": "r42", "message": "..."}
```

2. Apply-time (arrives ~2 sim frames later):
```json
{"type": "result", "id": "r42", "status": 0, "applied_at_frame": 220, "verb": "train"}
```

The `verb` field echoes what the client sent (useful when the
client has shed its outbound record by the time the result
arrives). `applied_at_frame` is the sim frame at which the sim's
`read_action` ran on this command.

## Status vocabulary (v1)

Only apply-time codes (0..99) travel on `result` messages.
Enqueue-time failures use `type: "error"` instead; they never
reach a `result`.

| id | name             | when                                                                            |
|----|------------------|---------------------------------------------------------------------------------|
| 0  | `APPLIED`        | Sim's `read_action` returned true — the action ran and mutated state           |
| 1  | `REFUSED`        | Sim's `read_action` returned false — engine validation rejected                |
| 2  | `THROWN`         | `read_action` threw a `bwgame::exception` at apply site                        |
| 3  | `SLOT_INACTIVE`  | Actor slot went inactive between schedule and apply (rare; sync.h ~line 2170)  |
| 4  | `NEVER_APPLIED`  | Game ended or agent disconnected before the tick came due                      |

Status IDs 100+ are reserved and unassigned; the server will not
emit them today. Clients should treat any status ≥ 100 as
"non-terminal / lifecycle" and not as APPLIED.

## Common cases

**`APPLIED`** — the happy path. The unit obeyed. State mutations
(minerals debited, unit trained, building placed, etc.) are visible
in the next observation.

**`REFUSED`** covers everything the sim's per-action validator
rejects:

- Actor unit doesn't exist (dead, invalid id, wrong owner)
- Target unit doesn't exist / wrong team / out of range
- Insufficient minerals / gas / supply
- Missing prerequisite (build a Barracks with no Command Center, etc.)
- Wrong-race verb (Terran asking to `morph_building`)
- Addon-already-attached / tank-not-in-tank-mode / spider-mine-inventory-empty
- Building placement invalid at (tile_x, tile_y)

The v1 vocabulary does not distinguish sub-cases of REFUSED. If an
agent needs the finer distinction, it must inspect state via
`observe()` after the REFUSED. Extending sub-codes would require
patching every `action_*` function in `actions.h` to return an
enum instead of `bool`; deferred pending demand.

**`THROWN`** should be rare in practice — it means one of the
`error("...")` calls in `actions.h` fired. Historically these mean
one of:
- unknown action opcode
- malformed action payload
- selection integrity check failed

If your agent sees `THROWN` in production, it's a bug on OUR side,
not yours. Report it with the sync-log or a repro command.

**`SLOT_INACTIVE`** — the actor's controller became `inactive`
mid-tick (typically because it just lost the game — no units left).
The command doesn't apply; the sim discards remaining scheduled
actions for that slot.

**`NEVER_APPLIED`** — the client's tick never came around (game
ended first, or the connection died before the sim advanced far
enough). The Python client synthesizes this locally when the reader
loop drains after WS close; the server never sends it as a wire
message.

## Timing

The result message arrives ~2 sim frames after the ack. In wall
clock:

| game-speed | ms/frame | 2-frame latency |
|-----------|----------|-----------------|
| turbosuper (10)   | 10 ms  | ~20 ms |
| superfast (20)    | 20 ms  | ~40 ms |
| fastest (42)      | 42 ms  | ~84 ms |
| fast (56)         | 56 ms  | ~112 ms |
| normal (67)       | 67 ms  | ~134 ms |
| slow (83)         | 83 ms  | ~166 ms |
| slower (111)      | 111 ms | ~222 ms |
| slowest (167)     | 167 ms | ~334 ms |

The `python_agent.client.Client.cmd()` default timeout is 3s —
comfortable margin for the slowest game-speed plus network jitter.
Override per call: `await c.train(u, t, timeout=1.0)`.

## Python usage

```python
from python_agent.client import Client, CommandTimeout
from python_agent.status import Status

async def main():
    c = await Client("sk-...").connect()
    r = await c.train(barracks_id, Terran_Marine)
    if r.ok:                        # r.status is Status.APPLIED
        print(f"applied at frame {r.applied_at_frame}")
    elif r.status is Status.REFUSED:
        print("sim refused (probably no minerals or dead barracks)")
    elif r.status is Status.THROWN:
        print("server-side action-decoder error; file a bug")
    # CommandTimeout raises on wall-clock timeout, not caught here.
```

The `CommandResult` dataclass:

```python
@dataclass
class CommandResult:
    status: Status                       # IntEnum, 0..4 in v1
    applied_at_frame: int
    verb: str                            # server echo
    id: str                              # correlation id
    queued_at_frame: int | None = None   # from the ack, or None if missed

    @property
    def ok(self) -> bool: ...            # True iff status is APPLIED
```

## Wire compatibility

Adding new status IDs is backwards-compatible for the wire — old
clients that don't recognize a new code see it as an unknown int
and (in the Python client's default handler) fall through to
`Status.REFUSED` so they don't mistakenly treat it as APPLIED.

Adding new terminal-blob-type verbs is also backwards-compatible:
the server ships a `result` for every accepted `cmd`, regardless of
verb.

Removing a code is a breaking change. Don't.

## Migration notes for existing agents

Every existing `await c.train(...)` / `c.build(...)` / etc. call
now blocks ~2 sim frames longer (waiting for the result instead of
returning at ack-time). At the default `--game-speed 10` this is
~20 ms per call — well within the ~1.5 s decision cadence used by
p_agent_v4, t_agent_v6_*, z_agent_v5.

The return type changed from `dict` (the raw ack JSON) to
`CommandResult`. Callers that used the return value:

- Before: `ack = await c.train(...); print(ack["queued_at_frame"])`
- After:  `r = await c.train(...); print(r.queued_at_frame)`

Or ignore the return entirely (common in existing code — most
callers only wanted the fire-and-forget) — no code change needed.

The existing per-agent "did my train happen?" heuristics can stay
as belt-and-suspenders; the new `result` gives a more reliable
signal but doesn't require removing legacy code. That's a
per-agent cleanup, not blocking.
