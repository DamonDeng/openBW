# simsc_app — Qt6 native spectator

Native macOS Qt6 desktop client that spectates an openbw game over
QWebSocket. Replaces the browser popup + socat proxy stack for local
desktop use.

## Day 1: headless smoke (this drop)

Boots a QCoreApplication, opens a QWebSocket to a `--url` you supply,
runs `funcs.next_frame(server)` on a 10 ms QTimer, prints per-frame
progress lines. No rendering yet — Day 2 adds the QWidget game
surface, Day 3 wraps it with a connect dialog.

The Day 1 target proves the new transport (`sync_server_qt_ws.h`)
is a byte-compatible drop-in for `sync_server_asio_ws.h`: same
duck-typed template contract, same wire format, so sync.h talks to
it without change.

## Build

```
cmake -S . -B build_qt \
    -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@6 \
    -DOPENBW_BUILD_SIMSC_APP=ON \
    -DOPENBW_BUILD_OBSERVER=OFF -DOPENBW_BUILD_SERVER=OFF
cmake --build build_qt --target simsc_app -j
```

The `OPENBW_BUILD_SIMSC_APP` option is off by default so headless
CI images (server-only) don't need Qt installed.

## Smoke test

Terminal 1 — game server (wait for our observer to join before
starting the sim, avoids the late-join replay corner):

```
./build_srv/server/openbw_server \
    --map "/path/to/(2)Bottleneck.scm" \
    --data-path /path/to/starcraft/resources \
    --no-auth --obs-port 6114 --wait-observers 1 \
    --game-speed 10 --fixed-initial-rand 12345678
```

Terminal 2 — the Qt spectator (same map path, same data path):

```
build_qt/simsc_app/simsc_app \
    --data-path /path/to/starcraft/resources \
    --map "/path/to/(2)Bottleneck.scm" \
    --url ws://127.0.0.1:6114/observer \
    --max-frames 900
```

Expected: `[simsc_app] frame=300 clients=2 viewing_slot=-1` etc.,
exits at `--max-frames`, server tail shows `observers=1` throughout.
