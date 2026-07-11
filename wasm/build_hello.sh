#!/bin/bash
# Phase 1 hello-world build. Produces dist_hello/{hello.html,hello.js,hello.wasm}.
# Run `cd dist_hello && python3 -m http.server 8000` and open
# http://localhost:8000/hello.html to see a bouncing cyan rectangle.

set -e

cd "$(dirname "$0")"
mkdir -p dist_hello

# Note on -sALLOW_MEMORY_GROWTH: at emscripten 6.0.2, enabling growth makes
# the wasm memory a *resizable* ArrayBuffer. Firefox implements the current
# ECMAScript spec strictly, and rejects TextDecoder.decode() on views of
# resizable buffers -- which trips emscripten's internal UTF8ToString right
# during startup (see setting the window title). Chrome is looser and works
# either way.
#
# For this hello-world we don't need growth (fixed 64MB is plenty), so
# leave it off. The real observer build will need growth AND a fix -- we'll
# either upgrade emscripten past the bug or pass -sTEXTDECODER=0 to force
# emscripten to use a JS-side polyfill that doesn't touch TextDecoder.
emcc hello_sdl.cpp \
    -o dist_hello/hello.html \
    -s USE_SDL=2 \
    -O2 \
    -sINITIAL_MEMORY=67108864

echo "Built dist_hello/. Serve with: (cd dist_hello && python3 -m http.server 8000)"
echo "Then open http://localhost:8000/hello.html"
