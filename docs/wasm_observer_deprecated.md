# WASM observer — deprecated 2026-07-27

The browser-hosted WASM observer (wasm/observer_wasm.cpp,
simsc/app/static/simscapp/observer.html) is deprecated and no longer
receiving fixes. New observer work targets the native Qt app
(simsc_app/) which will grow a lobby view alongside its existing
spectator view. WASM assets stay in the repo for now — no rip-out.

## Why we're stepping away

Two reasons, listed in order of weight:

### 1. Unresolved crash with the frame-batch server

Symptom: an EKS game pod running openbw_server with `--batch-size 24`
(the default injected by simsc/app/services/k8s_client.py) exits
with SIGSEGV (exit code 139) shortly after a WASM observer opens
and closes its tab. Reproduced three times on 2026-07-26 / 07-27
against the live EKS cluster.

What we ruled out with local repro attempts on 2026-07-27:

- **Not "any legacy client on a batched server"**: local
  openbw_server with --batch-size 24 + local Qt observer running
  *without* `--client-batch-size` (legacy per-tick mode) → four
  connect/disconnect cycles, zero crashes.
- **Not "any client on EKS"**: same EKS pod + local Qt observer
  in batch mode → connect/disconnect/reconnect cleanly, no crash.
  Local Qt observer against EKS *in legacy mode* — untested
  and probably worth trying later if this comes back.

So the trigger is specifically **WASM observer against
openbw_server built with the frame-batch commit** (b4bc068). The
crash always lands while `observers=1` is still true — the pod
log ends silently mid-tick, no assertion, no C++ exception
propagation, no goodbye line. That points at either:

- a use-after-free in `sync_server_asio_ws.h`'s
  `on_kill` firing during a live async I/O op (the
  `async_count` refcount tracking around `kill_client(c)` +
  `async_release(c)` is subtle), or
- something in TLS-terminated WS close from the browser (via
  ALB) that Qt's WebSocket close doesn't do — a text-frame,
  a close code with reason payload, ping-during-close, etc.

Full pod log for the last crash was saved to
`/tmp/crash_analysis/g-0904a3fd.log` on mingxuan's laptop; if we
ever return to this, that's the starting point. See
`docs/observer_frame_batching.md` for the batch protocol design.

### 2. WASM is too heavy for what we get

Even before the crash, the WASM observer has structural costs
that add up:

- **~30 MB payload**: `observer_wasm.wasm` + `.js` glue + the
  MPQ data files streamed in via IndexedDB. First load is
  ~30 s on the Beijing↔Tokyo link.
- **CPU-bound sim runs in the browser tab**: the whole openBW
  sim is compiled to WASM and runs client-side; a background
  tab throttles it, dropping frames.
- **Debug loop is painful**: no easy native debugger,
  emscripten symbolication is best-effort, and reproducing a
  network condition requires the actual EKS stack.

The native Qt observer (`simsc_app/`) is already faster,
smaller, and debuggable with lldb. Doubling down on it is
the higher-leverage direction — that's where the lobby is
going next.

## What stays in the repo

- `wasm/observer_wasm.cpp` + `wasm/build_observer_wasm*.sh`:
  source and build scripts, unchanged.
- `wasm/dist/`: pre-built WASM assets, still served by
  simsc's static handler.
- `simsc/app/static/simscapp/observer.html`: shell page that
  wraps the WASM module for browser use.

Nothing is deleted. If we want to revive WASM later — for a
demo, for auditability, or because the native app can't reach a
target audience — the code path is intact. It's just not on the
happy path anymore.

## What replaces it

Native Qt spectator + lobby in `simsc_app/`. Planned scope:

1. Lobby screen (talks to simsc REST): list running games,
   create game, accept invitation, jump into spectate.
2. Existing observer view (unchanged) as the in-game render.
3. API-key auth stored in QSettings — no Cognito browser popup.

Design doc for the lobby is not yet written; will land under
`docs/simsc_app_lobby.md` when the shape settles.

## Cross-refs

- Frame-batch protocol: `docs/observer_frame_batching.md`
- WASM observer's original design: `docs/wasm_observer_poc.md`
- The commit that introduced the crash-triggering server change:
  `b4bc068` (sync.h: id_frame_batch protocol for observer jitter smoothing)
