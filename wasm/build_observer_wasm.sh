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
    # Bake all of original_resources/ into a .data blob at the same
    # path in the emscripten VFS. observer_wasm.cpp reads relative
    # paths like "original_resources/(2)Bottleneck.scm".
    --preload-file original_resources
    # Emscripten's default HTML shell is fine for the PoC; we'll swap
    # in a custom shell in Phase 4 (form for host/port + api key).
)

echo "==> emcc compiling ${SOURCES[*]}"
emcc "${SOURCES[@]}" \
    "${INCLUDES[@]}" \
    "${DEFINES[@]}" \
    "${FLAGS[@]}" \
    "${EMSCRIPTEN_FLAGS[@]}" \
    -o wasm/dist/observer_wasm.html

echo "==> Built wasm/dist/observer_wasm.{html,js,wasm,data}"
echo "    ls -la wasm/dist"
ls -la wasm/dist
echo "==> To serve:"
echo "    cd wasm/dist && python3 -m http.server 8123"
echo "    open http://localhost:8123/observer_wasm.html"
