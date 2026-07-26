# Observer frame-batching protocol — design

**Status**: proposed  
**Author**: mingxuan + Claude collab  
**Date**: 2026-07-26  
**Depends on**: sync.h agent-batch protocol (SyncBreaker #8 fix,
2026-07-14 in commit `6962145`).

---

## Problem

External observer clients connect to the game server over WSS
through a long-haul network (client in Beijing, server on EKS in
`ap-northeast-1` Tokyo). Baseline round-trip is ~340 ms.

The current sync protocol sends the client one message per
sim tick:

- `id_client_frame` heartbeat (server tick complete)
- `id_agent_action_batch` (all agent commands from that tick)

At retail "fastest" speed (24 FPS = 42 ms/tick) that's 24
messages/sec. Any burst of network jitter or TCP-buffer coalescing
delivers 1-3 ticks late, then the next few arrive back-to-back. The
observer's sim runs step-locked to incoming heartbeats, so bursts
look to the user as a **stutter followed by fast-forward**.

Measured on `simsc.agentnumber47.com`, 2026-07-26:

| percentile | tick interval (server heartbeat receive-to-receive) |
|---|---|
| p50 | 42 ms (target) |
| p95 | 46 ms |
| p99 | 56 ms  ← 14 ms over budget, visible stutter |
| max in run | 56 ms |

The 1% of ticks that arrive ~14 ms late produce a noticeable hitch
every few seconds.

## Insight

The **server sim does not need to be pace-locked to the observer's
wall clock**. The server plays its own game (agents drive commands,
in-process, zero latency to sync). Observers only need to *watch*
what happened — the "when" of arrival at the observer can be
smoothed if the server ships bulk playback data.

## Proposed protocol

### New message: `id_frame_batch` (server → observer)

Server accumulates N sim ticks of activity, then flushes one batch
message covering that entire span:

```
[uint8_t  id_frame_batch]
[uint32_t start_frame]        server_frame of the first tick in the batch
[uint16_t n_ticks]            usually equal to server config --batch-size
[uint16_t n_actions]          total across all ticks in this batch
[for each action]:
    [uint32_t abs_frame]      the server tick this action applied on
    [uint8_t  slot]
    [uint16_t action_len]
    [action bytes ...]
```

Observer processes a batch as follows:

1. Enqueue it in a FIFO ordered by `start_frame`.
2. Local sim ticks at 42 ms/frame (same as retail). Each tick,
   look up the current `sync_frame` in the queue's head batch,
   apply any actions whose `abs_frame == sync_frame`, then advance.
3. When we've drained the head batch (our sync_frame reached
   `start_frame + n_ticks`), pop it and continue with the next.
4. If the queue is empty, pause the sim — we're waiting for
   the next batch. When it arrives, resume.

Guarantee: as long as one full batch is buffered ahead of our
playback, we never see a stutter shorter than the batch duration.

### New message: `id_capabilities` (client → server, after `id_auth`)

Capability bit set advertises support for `id_frame_batch`.
Payload:

```
[uint8_t  id_capabilities]
[uint16_t caps_bitmask]        bit 0 = supports id_frame_batch
```

Server tracks capabilities per client. If a client did NOT send
`id_capabilities` or did not set bit 0, the server continues to
emit per-tick `id_client_frame + id_agent_action_batch` as today.
If the client did opt in, the server suppresses those two messages
for that client and emits `id_frame_batch` instead.

The old and new protocols coexist so we can roll the server without
rebuilding every observer.

### CLI: server `--batch-size N`

Server config. Default = 24 (1 sec of retail time). Range 1..240.

Setting `--batch-size 1` disables batching (equivalent to legacy
behavior). Useful for latency-critical local testing.

## Server implementation sketch

Per client that opted in, the server maintains:

```cpp
struct client_batch_state {
    uint32_t batch_start_frame;      // server_frame where this batch began
    uint16_t n_ticks_pending;        // 0..batch_size
    std::vector<uint8_t> action_buf; // serialized entries for THIS batch
    uint16_t n_actions;              // count in action_buf
};
```

Inside `sync_next_frame()` after per-tick agent-action processing:

1. For each opted-in client:
   - Append any agent actions from this tick to `action_buf`
     (each entry: `[abs_frame][slot][action_len][bytes]`).
   - Increment `n_ticks_pending`.
   - If `n_ticks_pending == batch_size`: build and send the
     `id_frame_batch` message, reset the accumulator (new
     `batch_start_frame = current server_frame + 1`).

For legacy clients: existing code path unchanged — `send_client_frame`
and `broadcast_agent_action_batch` fire per tick.

## Observer client sketch

New state on `sync_state`:

```cpp
struct frame_batch_t {
    uint32_t start_frame;
    uint32_t n_ticks;
    std::vector<pending_batch_action> entries;  // reuse existing type
};
std::deque<frame_batch_t> batch_queue;
uint32_t batch_playback_frame = 0;  // next tick to render
bool batch_mode = false;             // set on id_capabilities ACK
```

Message handling in `process_messages`:

```cpp
case sync_messages::id_frame_batch: {
    frame_batch_t b;
    b.start_frame = r.get<uint32_t>();
    b.n_ticks = r.get<uint16_t>();
    uint16_t n = r.get<uint16_t>();
    b.entries.reserve(n);
    for (auto i = 0; i < n; ++i) {
        pending_batch_action e;
        e.frame = r.get<uint32_t>();
        e.slot = r.get<uint8_t>();
        uint16_t len = r.get<uint16_t>();
        e.data.resize(len);
        r.get_bytes(e.data.data(), len);
        b.entries.push_back(std::move(e));
    }
    sync_st.batch_queue.push_back(std::move(b));
    break;
}
```

In `all_clients_in_sync()` on the client:

```cpp
if (batch_mode) {
    // We're "in sync" iff our sim frame is inside the head batch's
    // range. If the queue is empty, we're stalled -- return false
    // and wait.
    if (batch_queue.empty()) return false;
    auto& head = batch_queue.front();
    return sync_frame >= head.start_frame
        && sync_frame < head.start_frame + head.n_ticks;
}
// else: legacy path
```

The 42-ms QTimer in `simsc_app/main.cpp` unchanged. As long as
`batch_queue` has a batch, `all_clients_in_sync()` returns true and
the local sim advances one tick. Actions with `abs_frame == sync_frame`
are drained from the head batch and applied through the existing
`schedule_action` path.

## Compatibility matrix

| Server | Client | Behavior |
|---|---|---|
| new (bumps batch_size default to 24) | old observer | per-tick as today — no regression |
| new | new observer | batched — smooth playback |
| old (still deployed somewhere) | new observer | falls back to per-tick — new client's `id_capabilities` is ignored by old server |

## Trade-offs

**Wins**:

- Observer feels smooth even with p99 network jitter up to `batch_size * 42 ms` (default: 1 sec).
- Fewer WS messages per second on the wire; less ALB / TLS overhead.
- Server sim decouples from observer wall clock — server can even
  run faster than realtime and observers still see correct pacing.

**Costs**:

- Observer sees the world `batch_size * 42 ms` behind the server.
  Default 1 sec of lag. Acceptable for spectator use.
- Agent clients (in-process to server) are NOT batched — they
  need to react per-tick.
- One-time protocol expansion. Requires client opt-in via
  `id_capabilities`. Old builds keep working.
- Server memory: one accumulator per opted-in observer.
  ~8 KiB peak per client for typical batch sizes; trivial.

## Rollout plan

1. **Server change first**, deployed with legacy path still enabled
   for un-upgraded clients. Test on staging: old observers still
   work, no regressions.
2. **Client update**: simsc_app (Qt), then WASM observer, then SDL
   observer. Each sends `id_capabilities` and enters batch mode.
3. **After all clients upgraded**: legacy path can eventually be
   removed. Not urgent; the code cost of keeping both is small.

## Non-goals

- Batching agent actions across ticks (would break reactivity).
- Recompressing action bytes (they're already tightly encoded).
- Adaptive batch sizing (defer until we see actual load patterns
  that demand it).
- Replacing the sync.h `id_client_frame` heartbeat protocol
  wholesale (staying additive minimizes risk).

## Open questions

- **What happens if the observer's sim runs faster than realtime**
  (say the user maxes out a debug knob)? Answer: irrelevant —
  the batch queue has a fixed 1-sec buffer; the observer sim
  stalls at the batch boundary until the next batch arrives, so
  the observer can never outrun the server.

- **What about `id_insync_check` hashes?** These fire every 32
  ticks in the current protocol. They arrive as separate messages
  and are compared against local state after each tick. Batching
  doesn't change this — the check still runs at frame 32/64/96/...
  regardless of when the containing batch arrives.

- **Should the server include `id_insync_check` inside the batch?**
  Slightly cleaner; also slightly more code. Ship without and
  revisit if the check falls out of sync with batched playback.
