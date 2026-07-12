# Docker image for openbw_server

`Dockerfile.server` at the repo root produces a container image for the
headless game server. Aimed at EKS / any k8s-compatible runtime;
locally usable via `docker run`.

## What's in the image

- **Base**: `debian:12-slim` (bookworm, glibc). ~97 MB.
- **Binary**: `/opt/openbw/bin/openbw_server` — the CMake `openbw_server`
  target, `strip`ped. ~1.2 MB.
- **Game data**: `/opt/openbw/data/` — the three MPQ files
  (`StarDat.mpq`, `BrooDat.mpq`, `Patch_rt.mpq`) plus every map from
  `original_resources/` (44 `.scm` files across 2/3/4/6/8-player maps,
  plus `campaign/`, `ladder/`, `oldladder/`, `scenario/` subdirs).
  ~125 MB.
- **Total image size**: ~223 MB uncompressed.

The **ENTRYPOINT** is `/opt/openbw/bin/openbw_server --data-path
/opt/openbw/data` so pod / `docker run` args land directly on the
server without a shim. The default **CMD** is `--help`, so
`docker run <image>` with no args prints usage.

## Build

```
docker build -f Dockerfile.server -t openbw-server:local .
```

~22 s on Apple silicon (arm64), assuming the debian:12-slim base is
cached. First-time pull adds ~10 s.

## Run

Bare help:
```
docker run --rm openbw-server:local
```

Two-player Bottleneck with inline users:
```
docker run --rm -p 6113:6113 -p 6114:6114 openbw-server:local \
  --map '/opt/openbw/data/(2)Bottleneck.scm' \
  --user 'alice:sk-testkey-alice-abcdefghijk:player:0' \
  --user 'bob:sk-testkey-bob-abcdefghijklm:player:1' \
  --user 'spec:sk-testkey-spec-abcdefghijk:observer' \
  --game-speed 42
```

- `6113` = agent WebSocket (JSON control protocol)
- `6114` = observer WebSocket (sync.h binary frames)

From another terminal, a Python agent authenticates using the alice key:
```
python3 -m python_agent.agents.p_agent_v4 \
  sk-testkey-alice-abcdefghijk \
  --host 127.0.0.1 --port 6113 --interval-sec 1.0
```

Or point a local `openbw_observer` binary at the container:
```
./build_srv/ui/openbw_observer \
  --map 'original_resources/(2)Bottleneck.scm' \
  --data-path original_resources \
  --server 127.0.0.1:6114 \
  --api-key sk-testkey-spec-abcdefghijk
```

## Kubernetes pod spec

The image is designed to be launched by a control API's `Pod` create
call. Every game-specific parameter is a CLI arg — no ConfigMaps,
Secrets, or mounted volumes required for the base case.

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: game-abc123
  labels:
    app: openbw-server
    game-id: abc123
spec:
  containers:
    - name: openbw-server
      image: openbw-server:local     # or an ECR ref in prod
      args:
        - "--map"; - "/opt/openbw/data/(6)New Gettysburg.scm"
        - "--user"; - "alice:sk-...:player:0"
        - "--user"; - "bob:sk-...:player:1"
        - "--user"; - "carol:sk-...:player:2"
        - "--user"; - "spec:sk-...:observer"
        - "--race"; - "0=protoss"
        - "--race"; - "1=zerg"
        - "--game-speed"; - "42"
      ports:
        - name: agent-ws
          containerPort: 6113
        - name: observer-ws
          containerPort: 6114
      resources:
        requests: { cpu: "200m", memory: "256Mi" }
        limits:   { cpu: "1000m", memory: "1Gi"  }
  restartPolicy: Never   # game ends -> pod dies
```

## Design notes

### Why args instead of a mounted users.json

The control API produces a fresh set of API keys per game (from
`openbw_keygen` or its own CSPRNG). Baking them into args:
- Keeps the pod stateless with respect to disk — no volume mounts,
  no ConfigMaps to track.
- Keys die with the pod; blast radius is one game.
- Fits a purely-API-driven control plane: to start a game the API
  synthesizes a `Pod` object with the args populated and calls
  `create`, nothing else.

**Trade-off**: CLI args are visible in `/proc/<pid>/cmdline` on the
node, so anyone with `kubectl exec` into a co-scheduled pod on the
same node could `ps auxww` and see the keys. For a per-game
ephemeral pod with per-game keys this is acceptable. If you need
tighter secrecy later, `--user` is easy to swap for env-var
loading; see `server/auth.h::add_from_spec` and the equivalent
env-var loader hook.

### Why data is baked into the image, not on a volume

Maps and MPQs are read-only, ~125 MB, and change roughly never.
Baking them in means:
- No PersistentVolume to provision.
- No init-container to download data.
- Pod cold-start = image-pull + binary-start. Nothing else.

If you want to add new maps, rebuild the image with the extra map
files in `original_resources/`.

### Warm vs cold pod start

- **Warm** (image cached on node, node has room): ~5-10 s from
  `kubectl apply` to `--user` auth accepting a connection.
- **Cold** (node scale-out via Karpenter): 60-90 s, dominated by
  EC2 provisioning + ECR image pull.

Keep a small pool of warm nodes to make the warm path the common
one. See `docs/soak_testing.md` and the SB#5 handoff for the sim's
own timing characteristics.

## Multi-arch builds

The default `docker build` produces the host's native arch (arm64
on Apple silicon, amd64 on x86 Linux). EKS worker nodes are
usually amd64; use `buildx` for multi-arch:

```
docker buildx build --platform linux/amd64,linux/arm64 \
    -f Dockerfile.server -t openbw-server:local .
```

## What's NOT in the image

- **The observer client** (`openbw_observer` SDL2 binary). Observers
  are end-user clients — either the native SDL2 binary running on
  the user's laptop, or the WASM build embedded in a web page.
- **Any Python agent code**. Agents live outside the game pod; they
  connect over the network like observers do.
- **users.json**. Auth is passed via `--user` args at run time. The
  file-based flag `--users <path>` still works if you mount a
  volume containing a JSON file, but that's a legacy dev path.

## Related tasks

- #121 Docker image for game server (this doc)
- #120 --user CLI flag (dependency)
