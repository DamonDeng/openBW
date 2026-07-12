#!/bin/bash
# Phase 3 build: observer_wasm.cpp + ui/sdl2.cpp -> wasm.
# No networking yet; produces a browser observer that opens a canvas,
# loads the (2)Bottleneck map, and renders the initial frame.

set -e

cd "$(dirname "$0")/.."   # repo root
mkdir -p wasm/dist

SOURCES=(
    wasm/observer_wasm.cpp
    ui/sdl2.cpp
)

# -I. so #include "ui.h" resolves to ui/ui.h via the ui/ dir listed below,
# and #include "../bwgame.h" (from ui/*.h) resolves to bwgame.h at repo
# root. -Iui so observer_wasm.cpp's #include "ui.h" finds it directly.
INCLUDES=(
    -I.
    -Iui
)

# No asio, no ASIO_STANDALONE -- we're not linking a sync transport in
# Phase 3. This shrinks compile time noticeably and drops the poll.h
# warning the Phase 2 build showed.
DEFINES=(
    -DOPENBW_NO_SDL_MIXER
    -DOPENBW_NO_SDL_IMAGE
    -DOPENBW_ENABLE_UI
)

FLAGS=(
    -std=c++14
    -O2
    -Wno-deprecated-declarations
)

EMSCRIPTEN_FLAGS=(
    -sUSE_SDL=2
    # 256MB fixed. Growth (-sALLOW_MEMORY_GROWTH) creates a resizable
    # ArrayBuffer that trips Firefox's TextDecoder in emscripten 6.0.2
    # (same Phase 1 issue). 256MB is enough headroom for one live game.
    -sINITIAL_MEMORY=268435456
    # File API + module-runtime exports so JS can call in later.
    -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS']
    # Link emscripten's WebSocket runtime -- provides
    # emscripten_websocket_new / _send_binary / callback setters used
    # by sync_server_emscripten_ws.h. Without this the linker errors
    # on those symbols.
    -lwebsocket.js
    # ASYNCIFY: lets C++ code call emscripten_sleep() to yield to the JS
    # event loop mid-function. sync_server_emscripten_ws.h uses this in
    # run_one/run_until so sync.h's "block until server heartbeat" pattern
    # actually blocks (yielding to JS) instead of racing ahead. Without
    # this the local sim runs at 60fps regardless of server pace and all
    # agent-actions apply to the wrong (future) frames -- see the
    # 2026-07-11 pacing bug.
    -sASYNCIFY=1
    # ASYNCIFY inflates code size + adds a ~10-20% runtime overhead;
    # for a PoC that's fine. Later optimizations: pin a subset via
    # -sASYNCIFY_ADD or migrate to jspi.
    -sASYNCIFY_STACK_SIZE=32768
    # NO --preload-file: as of M5.a-mpq the MPQs + map file are
    # supplied by the end user, cached client-side in IndexedDB,
    # and injected into the emscripten VFS via Module.preRun.
    # See simsc/app/static/simscapp/observer.html.
    # Rationale: (a) MPQ contents are Blizzard-owned so we can't
    # ship them from our servers, and (b) the 120 MB blob was a
    # cold-cache tax on every fresh browser.
    # We do NOT use --shell-file. Instead we ship a plain
    # observer_shell.html that manually loads observer_wasm.js
    # via a script tag. That way we don't have to deal with
    # emscripten's shell-substitution markers ({{{SCRIPT}}} etc)
    # and the shell can define window.OPENBW_* globals BEFORE
    # the module loads.
)

echo "==> emcc compiling ${SOURCES[*]}"
emcc "${SOURCES[@]}" \
    "${INCLUDES[@]}" \
    "${DEFINES[@]}" \
    "${FLAGS[@]}" \
    "${EMSCRIPTEN_FLAGS[@]}" \
    -o wasm/dist/observer_wasm.html

# Copy our custom shell into dist/ so http.server serves it side-by-side
# with the generated .js/.wasm/.data. Emscripten's own observer_wasm.html
# also lands there but we don't use it.
cp wasm/observer_shell.html wasm/dist/index.html

echo "==> Built wasm/dist/observer_wasm.{html,js,wasm,data} + index.html"
ls -la wasm/dist
echo "==> To serve:"
echo "    cd wasm/dist && python3 -m http.server 8123"
echo "    open http://localhost:8123/"
