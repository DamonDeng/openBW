#!/bin/bash
# Phase 2 build: attempt to compile openbw_observer to wasm.
# Iterative -- we'll patch things as errors surface.

set -e

cd "$(dirname "$0")/.."   # repo root
mkdir -p wasm/dist

# Common flags:
#   -sUSE_SDL=2                emscripten's SDL2 port
#   -sINITIAL_MEMORY=256M      openbw needs a lot; without growth (Firefox bug)
#   -DOPENBW_NO_SDL_MIXER      no audio
#   -DOPENBW_NO_SDL_IMAGE      no PNG image lib (already the default for native)
#   -DOPENBW_ENABLE_UI         hint used inside the codebase
#   -O2                        release-ish; helps compile speed too
#   -std=c++14                 matches ui/CMakeLists.txt
#   -Wno-*                     silence noise; we'll fix real errors

# All the source we'll try to compile in one shot.
SOURCES=(
    ui/observer.cpp
    ui/sdl2.cpp
)

INCLUDES=(
    -I.
    -Ideps/asio     # observer.cpp still includes asio through sync_server_asio_ws.h
)

DEFINES=(
    -DOPENBW_NO_SDL_MIXER
    -DOPENBW_NO_SDL_IMAGE
    -DOPENBW_ENABLE_UI
    -DASIO_STANDALONE
)

FLAGS=(
    -std=c++14
    -O2
    -Wno-deprecated-declarations
)

EMSCRIPTEN_FLAGS=(
    -sUSE_SDL=2
    -sINITIAL_MEMORY=268435456                        # 256 MB
    -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS']
    --preload-file original_resources
)

echo "==> emcc compiling ${SOURCES[*]}"
emcc "${SOURCES[@]}" \
    "${INCLUDES[@]}" \
    "${DEFINES[@]}" \
    "${FLAGS[@]}" \
    "${EMSCRIPTEN_FLAGS[@]}" \
    -o wasm/dist/observer.html

echo "==> Built wasm/dist/. To serve:"
echo "    cd wasm/dist && python3 -m http.server 8123"
echo "    open http://localhost:8123/observer.html"
