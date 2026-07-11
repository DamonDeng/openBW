# WASM observer — Proof of Concept

## Goal

A browser-loaded openBW observer that connects to a locally-running
`openbw_server` and renders a live game. Success = "I can open a
tab, click connect, and watch the game exactly as I would in the
native `openbw_observer` window."

This is a **de-risking PoC**, not production code. We want to
learn what a WASM observer looks like, feels like, and costs to
build, before committing the hosted-platform architecture to the
assumption "observers run in the browser."

## Success criteria

In priority order:

1. **Builds.** Emscripten produces `.wasm` + `.js` from our
   existing C++ observer code.
2. **Renders.** SDL2 → HTML5 canvas shows sprites, terrain, fog,
   minimap — same as the native observer.
3. **Connects.** Browser `WebSocket` opens against `openbw_server`
   and streams the sync-transport protocol.
4. **Stays in sync.** With the SyncBreaker-fixed server code, the
   WASM observer's sim state stays byte-identical to a native
   observer running against the same game. Confirmed by diffing
   `--sync-log` outputs at 300-frame INVENTORY snapshots.
5. **Usable.** Smooth playback at `--game-speed=42` (24 FPS
   default). Doesn't have to be as fast as native; has to be
   watchable.

## Non-goals

- Portal integration (JWT auth, embed in Vue, etc.). PoC uses a
  plain HTML page with a URL-input form.
- Production polish (loading spinners, error toasts, replay
  scrubbing). Skip.
- Cross-browser certification. Chromium first; if Firefox/Safari
  crash we note it but don't chase.
- Mobile.
- WebGL / GPU rendering. Emscripten's SDL2 → 2D canvas is fine at
  this scale; GPU is a later optimization.

## Design decisions (locked before starting)

| Question | Choice | Rationale |
|---|---|---|
| Which observer to start from | Fork `ui/observer.cpp` + build with emscripten | Reuses ~95% of our rendering code; sim is portable C++ already. |
| SDL2 for the browser | Emscripten's `-s USE_SDL=2` port | First-class, well-supported; no reason to compile SDL from source. |
| MPQ data loading | Emscripten `--preload-file` for the whole `original_resources/` dir | Simplest thing that works. All ~50MB baked into one `.data` blob, browser caches it. Split per-MPQ later if load time hurts. |
| Sync transport | New `sync_server_emscripten_ws.h`, mirrors `sync_server_asio_ws.h`'s public API but wraps emscripten's WebSocket callbacks | Cleaner than pulling asio+emscripten-socket-emulation. Sync.h is already transport-polymorphic via templates. |
| Where the WASM connects (for PoC) | Directly to `ws://localhost:6114/observer?key=…` on the developer machine | No platform to speak of yet. Once portal exists, just change the URL. |
| Main loop | `emscripten_set_main_loop(cb, 0, 1)` with target FPS driven by server frame rate | Browsers cannot `sleep()`; must be event-loop-driven. |
| HTML shell | Minimal: a form with `host`, `port`, `api_key` inputs, a "connect" button, and a canvas below | We do not want to hand-roll a UI just to prove the sim works. |
| Repo location | `openbw/wasm/` inside the existing openbw repo | The WASM observer is a build artifact of the openbw engine, not part of the future platform. Belongs here. |
| Commit strategy | Land the PoC as one or two commits on master once it works | No point sitting on a branch; this is de-risking, not risky. |

## Files that will land

```
wasm/
├── build.sh                        # runs emscripten, produces dist/
├── CMakeLists.txt                  # emscripten target
├── observer_wasm.cpp               # observer.cpp adapted to
│                                     emscripten_set_main_loop
├── sync_server_emscripten_ws.h     # browser WebSocket transport
├── index.html                      # HTML shell: form + canvas
├── README.md                       # how to build, run, next steps
└── dist/                           # (not committed; build output)
```

## Sequence of work

Roughly one week of focused work. Each phase produces something
observable so we can stop and pivot if any phase gets ugly.

### Phase 1 — Emscripten hello-world

Install emscripten (`brew install emscripten` on macOS or a
python-managed emsdk install). Build a trivial C++ program with
SDL2 that draws a rectangle. Prove the toolchain works.

Halt if: emscripten setup takes > 1 day. Escalate for alternative.

### Phase 2 — openbw_ui compilable to wasm (rendering only, no networking)

Get the rendering pipeline (`ui.h`, sprite blit, tileset draw,
minimap) building under emscripten. This is the big compilation
exercise. Likely hits:

- Missing libc bits (patch or workaround).
- `a_vector` / custom allocators — should be portable.
- Any threading assumptions — I don't think we have any in the
  render path, worth confirming.
- File I/O expectations from `data_files_directory` when reading
  from an emscripten virtual FS.

Deliverable: `observer_wasm` binary that opens a canvas and
draws a black screen.

### Phase 3 — MPQ files loaded via `--preload-file`

Preload `original_resources/` into the emscripten virtual FS.
Trigger a map load. Confirm terrain + starting units render as
a static frame (no sim running yet).

Deliverable: browser tab shows a Bottleneck map frozen at frame
0 with starting units in place.

### Phase 4 — `sync_server_emscripten_ws.h`

Write the browser-side WebSocket transport. Same public API as
`sync_server_asio_ws` (bind, connect, new_message, send_message,
poll, set_on_message, set_on_kill, ...) but implemented via
emscripten's `emscripten_websocket_*` API. Server-side of this
transport is unused (browsers don't `bind`, only `connect`).

The bytes-in-frames semantic: emscripten delivers WebSocket
messages atomically, one per callback. Our sync.h wants a byte
stream. Adaptation layer buffers incoming frames.

Deliverable: observer_wasm connects to a running native server,
receives frames, sim advances, canvas updates.

### Phase 5 — `emscripten_set_main_loop`

Replace `while (true) { ... }` with a callback-per-frame. Frame
timing driven by server heartbeat, not local wall clock. Smooth
playback.

Deliverable: game plays out in the browser, at native FPS,
watchable end-to-end.

### Phase 6 — sync validation

The critical test: run one native observer and one WASM observer
against the same server. Both `--sync-log` on. After N minutes
(or on game end), diff the INVENTORY entries. Should be
byte-identical.

If they diverge: something in the WASM observer's sim differs
from native. Debug via the same tools we used for SyncBreaker
(sync-log AGENT_APPLY multiset + INVENTORY unit-count diff).

Deliverable: sync-log diff shows 0 real disagreements.

## Risks (in order of likelihood)

1. **Emscripten toolchain setup.** First-time integration always
   burns time. Budget half a day.

2. **MPQ reader semantics.** Our MPQ loader does synchronous
   seeks. Emscripten preload puts files in the virtual FS with
   full `fseek/fread` support, but corner cases (memory-mapped
   reads?) may need patches.

3. **WebSocket byte-stream semantics.** Emscripten delivers one
   message per callback. Our sync.h expects streaming bytes. Small
   adapter needed; low risk but must be right.

4. **`emscripten_set_main_loop`**. The observer's `while (true)`
   loop needs converting to a per-frame callback. Small
   refactor but touches the main entry point.

5. **Threading.** I don't think we have any observer-side
   threading, but if we do, emscripten pthreads require
   SharedArrayBuffer + COOP/COEP headers on the serving side.
   Complicates PoC. Verify no threads first.

6. **Performance ceiling.** WASM is ~50-80% of native. Rendering
   through canvas is fine at 24 FPS, unclear at 100 FPS. If
   speed=10 is choppy, we accept and note.

7. **Cross-origin WebSocket.** Serving HTML from
   `localhost:8080`, WS to `localhost:6114`. Browsers usually
   allow this but Chrome's Private Network Access can be
   finicky. Workaround: serve HTML on same origin as WS if it
   comes to it.

## What we learn from a successful PoC

- Concrete estimate of "how much C++ needs to change to make
  observer wasm-buildable" — informs future observer refactors.
- Load time budget on real hardware (~50MB `.data` blob).
- WebSocket message rate the browser can absorb.
- Whether the WASM path is a real product or a nice-to-have.

## What we learn from a failed PoC

Almost as valuable. Reasons a PoC could fail:

- **Emscripten can't build our C++** — we have to refactor big
  chunks of the engine, decision: is it worth it, or should we
  rewrite observer in TypeScript?
- **MPQ loading is too slow** — either preload or the file
  format itself is not browser-friendly, need an alternative
  asset pipeline (extract sprites/tilesets to raw PNG once,
  ship those; adds a build step but way faster loads).
- **Sync-log diverges** — WASM observer's sim doesn't match
  native. Deep problem, may indicate hidden non-determinism.
- **Frame rate below usable** — decision point on rendering
  backend (canvas vs WebGL vs skip WASM entirely).

Each failure mode gives us a real answer about the platform
design, so this PoC is a good use of time even if it doesn't
succeed on the first try.

## Deliverables to commit

Assuming success:

- `wasm/` directory with everything above.
- Updated `docs/wasm_observer_poc.md` (this file) with a
  post-mortem: what worked, what surprised us, what the file
  size + load time actually were, benchmark of FPS vs native.
- A commit or two on master.

If we choose to keep going: the PoC becomes the foundation of
the production WASM observer. If we don't: the code stays as
reference for a possible future attempt.

## Next steps after PoC

Only if PoC succeeds:

- Portal integration: JWT auth instead of URL-key input.
- Vue component that hosts the WASM canvas + connection lifecycle.
- Split preload bundles per-MPQ for parallel fetch.
- CDN hosting for the `.data` blob.
- Cross-browser testing.
- Replay playback (loads a saved replay file instead of a live
  connection).
