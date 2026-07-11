# SyncBreaker

Codename for the class of bugs where a peer joining a running openbw
game silently misses one or more actions that were broadcast to
earlier peers, causing its local sim to diverge from the server and
from any peer that joined earlier. **Silently** means: no crash, no
log warning, no error path — just wrong game state that only becomes
visible via per-frame INVENTORY diffs or long enough visual playback.

The name captures the danger: it *breaks* sync. Not a slow drift,
not a rendering hiccup — a real sim-state divergence, hidden inside
the join / handshake / catchup mechanism.

## Why it matters

- **The whole point of the openBW server + observer/agent
  architecture is deterministic replay.** Every observer must see
  byte-identical sim state to the server for the entire game. When
  SyncBreaker triggers, that invariant fails silently.
- **The symptom looks like a transport or rendering bug** at first
  glance. Observer window shows a smaller army than it should, or
  "misses" a morph. Easy to blame SDL rendering, network latency, or
  WebSocket framing. In the July 2026 debugging session we suspected
  SDL rendering, then WebSocket framing, then observer clock drift,
  before finding the real cause — because each of those *also*
  produces intermittent divergence-looking symptoms.
- **Reproduces intermittently**, depending on OS scheduling and
  timing of the observer's handshake. That makes it worse: one clean
  run at 10/10 does NOT prove the bug is gone. You need a repro-loop
  harness that runs many rounds and classifies pass/fail
  automatically.

## First instance — fixed 2026-07 (commit 0baaef9)

### Location in the code

Two independent code paths on the server were almost-but-not-quite
consistent:

- **Broadcast path**: `broadcast_agent_action()` fires at *schedule*
  time (`server/main.cpp` cmd_queue.drain callback). Action bytes go
  to every peer currently in `sync_st.clients` via `id_agent_action`.
- **Record path**: `replay_saver.add_action()` fires at *apply*
  time (`sync.h execute_scheduled_actions`), which is `latency=2`
  frames later. Action bytes land in `replay_saver.history`.

The `id_catchup_data` bundle sent to a late-joining observer is
built from `replay_saver.history`. So if an observer's `id_greeting`
handshake landed in the 2-frame window between broadcast and record:

- Live broadcast: fired to the OLD peer set, which did **not**
  include the new peer (they weren't in `sync_st.clients` yet).
- Catchup bundle: built from `replay_saver.history`, which did
  **not** yet contain the action (add_action hasn't fired yet).
- Result: the action is invisible to the new peer forever. Its sim
  is permanently one action behind, and drifts further as
  downstream actions produce different world states.

### Symptom footprint (from the July 2026 repro)

- Server INVENTORY at sync-log frame 21000, slot 0: `min=886`,
  Hatchery + Pool + Den + Sunken + Spore + Evo + 2 Extractors,
  22 Drones, 1 Hydralisk.
- Bob-observer INVENTORY at same frame, same slot: `min=871`,
  Hatchery + Evo + 1 Extractor only, 7 Drones. **Whole game's
  worth of state divergence.**
- Server SEND action count: 24722. Bob observer RECV/APPLY: 2444
  (a strict *subset* of server's stream, not a permutation, not a
  corruption).
- Alice observer (connected earlier, outside the race window)
  stays perfectly in sync with server.

### Repro conditions

Under the "agents-first, observers-later" ordering (agents start
firing before observers connect):

- Raw TCP transport: ~20% fail rate per 25-second round.
- WebSocket transport (`sync_server_asio_ws.h`): ~30% fail rate.
  Not a WS-specific bug; WS just widens the vulnerable window
  because HTTP upgrade takes longer than raw TCP connect.

The Bash script `scripts/repro_syncbreaker.sh` (see below) runs 10
rounds and classifies pass / fail-real / fail-cosmetic automatically.

### The fix

At the time a new peer's greeting is processed (`sync.h` inside the
`recv_uid` handler, right after sending `id_catchup_data`), walk
every server-side virtual client — those with `player_slot >= 0`
and `h == nullptr` — and for each pending `scheduled_action`,
synthesize an `id_agent_action` message addressed to the new peer:

    [u8 id_agent_action][u8 slot][u32 server_frame][action bytes]

where `server_frame = sa.frame - latency` (reconstructs the frame
the action was broadcast at, since `sa.frame` stores `target_frame`).

The observer's local sim receives it, schedules it at
`target_frame = server_frame + latency` — the same absolute frame
the server uses — and applies it byte-identically at that frame.

10/10 clean rounds on both raw TCP and WebSocket after the fix.

## Detection playbook

If you suspect a SyncBreaker instance:

**Do NOT stop at multiset-containment on AGENT_APPLY events.**
A SyncBreaker'd observer's applied byte stream is a strict *subset*
of the server's sent stream. Multiset-containment says "no divergent
bytes" and returns clean. The observer applied FEWER bytes than it
should have, and its sim state is silently wrong.

**Always cross-check with per-frame INVENTORY diff.** Classify:

- **Mineral / gas drift only, unit counts match** → cosmetic timing
  wobble (`sync-log off-by-one`; see `docs/syncbreaker.md` note
  below and the referenced feedback memory).
- **Unit counts differ** → real sim divergence. If the observer has
  strictly fewer units than the server (never more), and the
  numbers are consistent with "missed exactly N actions", this is
  a SyncBreaker signature.
- **Unit counts differ AND observer has more of something the
  server doesn't** → different bug. RNG desync, action injection,
  double-record, etc. Not SyncBreaker.

### Repro loop (`scripts/repro_syncbreaker.sh`)

A checked-in Bash harness that:
1. Starts the server.
2. Starts two agents (they play immediately with 0 observers).
3. Waits 10 seconds so the action log has meaningful catchup content.
4. Launches two observers 0.4 seconds apart.
5. Plays 25 seconds.
6. Kills everything.
7. Diffs the three sync-logs, classifying INVENTORY disagreements
   as `min-only` (cosmetic) or `REAL` (unit-count divergence).
8. Repeats N rounds and prints a pass/fail summary.

If any round returns `verdict=FAIL_REAL`, there is (or was) a
SyncBreaker instance in that build. Keep the logs from the
failing round for post-mortem — they're preserved automatically
under `${round}_FAIL_REAL/` while passing rounds are cleaned up.

## Prevention

- **New transports don't need SyncBreaker awareness** *if* the fix
  stays at the `sync.h` greeting-handler layer, above the transport.
  Keep it there. The transport should be a dumb byte pipe.
- **If you refactor how actions flow** from cmd_queue → broadcast →
  history, any reordering where broadcast happens before history is
  a potential SyncBreaker unless the greeting handler also ships
  pending state to the new peer.
- **Test with the repro loop before/after any change** to sync.h,
  server/main.cpp cmd_queue drain, or observer join handshake.

## Related but distinct

Three bugs in this codebase share the *phenotype* — silent state
divergence between server and observer — but have distinct root
causes. If you see a silent-drift symptom in the future, they're
the three candidate causes to rule in or out:

- **[Sync-frame offset bug](../README.md)** (commit c6e9427):
  `sync_frame` and `st.current_frame` were off by one at game start,
  causing scheduled actions to fire on the wrong sim frame.

- **Race-override observer desync** (commit 29607ee): server's
  `--race` CLI override never propagated to observers, so the
  observer's `start_game_impl` consumed extra `lcg_rand(144)`
  calls, silently desyncing the RNG stream. Fixed by shipping
  `slot_races` in the catchup bundle.

- **SyncBreaker** (commit 0baaef9): this document. The 2-frame
  broadcast-vs-record window.

When investigating a new silent-drift symptom, walk through these
three in order. If none fit, you have a new SyncBreaker variant to
add to this document.
