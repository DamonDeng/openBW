# SD-mode workshop deployment — server + client setup

**Audience:** teammate spinning up an openBW SD-mode workshop with a
remote game server plus a local Qt6 lobby + observer client on each
attendee's laptop.

**Scope:** exactly what you need to run a workshop. No HD assets, no
sprite viewer, no agent-development onboarding — just get people
into games. See `agent_readme.md` for agent authoring.

**What "SD mode" means here:** the classic StarCraft: Brood War
graphics rendered via openBW's own SDL/Qt pipeline. No SC:R install
required on attendees' laptops. Agents connect over WebSocket and
the observer window renders the same sim.

---

## 0. Prerequisites

**Server host** (single Linux VM or EKS pod):
- Docker 20+ **OR** ability to build from source (Debian 12 / Ubuntu 22 recommended)
- Ports **6113** (agent WebSocket) and **6114** (observer WebSocket) reachable from attendee laptops
- Optional: a DNS name pointed at the host (TLS termination via a reverse proxy is up to you — the openbw_server itself speaks plain WS, not WSS)
- StarCraft: Brood War data files (`StarDat.mpq`, `BrooDat.mpq`, `patch_rt.mpq`) — the same ones a retail SC install ships. Attendees do NOT need these; the server does.

**Attendee laptop** (macOS, Linux, or Windows via WSL — macOS is best-tested):
- Qt6 (>= 6.5) with `Core`, `Gui`, `Widgets`, `Network`, `WebSockets`
- CMake 3.16+, a C++17 compiler
- Their own StarCraft: Brood War data files (`StarDat.mpq`, `BrooDat.mpq`, `patch_rt.mpq`) — the desktop lobby needs these locally for the observer window's SD rendering
- Network path to the server host (port 6113 for agent, 6114 for observer)

**Two things attendees need from you before starting:**
1. The **server hostname or IP** (and ports if not default 6113/6114)
2. Their assigned **user aliases** (they choose their own API keys)

---

## 1. Server: build the openbw_server image

Two paths — pick one.

### 1a. Docker (recommended)

```bash
# From repo root
docker build -f Dockerfile.server -t openbw-server:workshop .
```

The Dockerfile is already set up for this: builds only `openbw_server`
(no observer, no Qt), bakes `original_resources/` (the SD MPQ set +
bundled maps) into `/opt/openbw/data/`, exposes 6113/6114, runs as
non-root user `openbw`.

Verify:

```bash
docker run --rm openbw-server:workshop --help
```

Expect the server's usage text. If you see it, the image is good.

### 1b. Build from source (if Docker isn't available)

```bash
mkdir -p build_srv && cd build_srv
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DOPENBW_BUILD_OBSERVER=OFF \
         -DOPENBW_BUILD_SERVER=ON
cmake --build . --target openbw_server -j"$(nproc)"
```

Result: `build_srv/server/openbw_server` binary. Copy this to the
target host together with `original_resources/` (the MPQs + maps).

---

## 2. Server: start one game

The `openbw_server` binary hosts exactly **one match**. To run many
concurrent games, launch multiple processes on different port pairs
(6113/6114, 6115/6116, …). For a workshop with N simultaneous games,
run N processes. Simpler than reusing one process across matches.

### 2a. Choose a map

Maps live under `/opt/openbw/data/` inside the container (or
`original_resources/` on disk if you built from source). Common
picks for 2-player workshop games:

- `(2)Bottleneck.scm` — small, fast games
- `(2)Boxer.scm`
- `(2)Challenger.scm`
- `(2)River Crossing.scm`

Full list: `ls original_resources/*.scm`.

### 2b. Define users

The server needs a **user spec per slot**. Format:

```
--user 'alias:api_key:role:slot'
```

- `alias` — attendee's display name (e.g. `alice`)
- `api_key` — a shared secret. Suggested format `sk-<random-token>`. Attendees pick their own; you never store or forward it. Keep it out of shell history for real workshops.
- `role` — always `player` (there's also `observer` but slot-bound observers aren't needed for a MOBA-style workshop — a spectator observer connects without a slot)
- `slot` — sim slot 0..7. In a 2-player match, use slots 0 and 1.

**Two players, Terran-vs-Zerg, on Bottleneck:**

```bash
docker run --rm -p 6113:6113 -p 6114:6114 openbw-server:workshop \
    --map '/opt/openbw/data/(2)Bottleneck.scm' \
    --user 'alice:sk-alice-workshop-key:player:0' \
    --user 'bob:sk-bob-workshop-key:player:1' \
    --race 0=terran --race 1=zerg \
    --game-speed fastest \
    --any-ws-path
```

Notes on the flags:
- `--race N=RACE` overrides slot N's race regardless of the map's default. Values: `zerg`, `terran`, `protoss`. `random` and `any` are rejected (exit 2) — callers (Qt lobby / simsc REST) resolve random before spawning openbw_server. This keeps the observer's local sim mirror deterministic without a re-derivation from the wire.
- `--game-speed fastest` runs the sim at retail's fastest speed (~24 fps). Options: `slowest`, `slower`, `slow`, `normal`, `fast`, `faster`, `fastest`, `fastest+`, `fastest++`, `fastest+++`, `fastest++++`. `fastest++++` is roughly 10ms/frame.
- `--any-ws-path` lets clients connect at any WS path (`ws://host:6113/anything`) instead of requiring `/agent`. Useful if you're behind a path-routing reverse proxy.
- `--data-path` is baked into the Docker ENTRYPOINT as `/opt/openbw/data`. If you're running the raw binary from source, add `--data-path <path/to/original_resources>`.

Server logs land on stderr. It stays running until the game ends
(one side eliminated or someone disconnects and the sim decides to
stop) or you kill the container.

### 2c. Multi-game deployment

For N parallel games, script the launch:

```bash
for i in 0 1 2 3; do
    port_agent=$((6113 + i * 2))
    port_obs=$((6114 + i * 2))
    docker run -d --name game-$i \
        -p $port_agent:6113 -p $port_obs:6114 \
        openbw-server:workshop \
        --map '/opt/openbw/data/(2)Bottleneck.scm' \
        --user "alice-g$i:sk-alice-g$i:player:0" \
        --user "bob-g$i:sk-bob-g$i:player:1" \
        --race 0=terran --race 1=zerg \
        --game-speed fastest --any-ws-path
done
```

Attendees connect to different port pairs per game. Announce the
port-to-game mapping ahead of time.

**If you already have an EKS deployment** (the private
`aws_account_info/` docs cover an existing simsc + openbw_server
setup at `simsc.agentnumber47.com`): the same `Dockerfile.server`
image is what runs there. Speak to the person who set that up for
workshop-scoping (path routing per game, ingress rules). This doc
covers standalone single-host runs.

---

## 3. Attendee laptop: build simsc_desktop

`simsc_desktop` is the end-user Qt6 app: it lists local + remote
games, spawns local `openbw_server` processes on demand, hosts
observer windows. For a **remote-server workshop** attendees only
need the "Remote Games" tab, but the same binary handles both.

### 3a. Install Qt6

**macOS:**
```bash
brew install qt@6 cmake
```

**Ubuntu / Debian:**
```bash
sudo apt install qt6-base-dev qt6-websockets-dev cmake build-essential
```

**Windows:** use the official Qt online installer, install the
Qt6.5+ MSVC or MinGW component, and open a Qt Command Prompt for
the build steps.

### 3b. Clone and build

```bash
git clone <this-repo-url> openbw
cd openbw
mkdir -p build_desktop && cd build_desktop
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DOPENBW_BUILD_OBSERVER=OFF \
         -DOPENBW_BUILD_SERVER=OFF \
         -DOPENBW_BUILD_SIMSC_DESKTOP=ON
cmake --build . --target simsc_desktop -j$(getconf _NPROCESSORS_ONLN)
```

macOS result: `build_desktop/simsc_desktop/simsc_desktop.app`.
Linux result: `build_desktop/simsc_desktop/simsc_desktop` binary.

### 3c. Place StarCraft data files

The lobby needs the retail SC:BW MPQs to render the observer's SD
sprites. Copy them somewhere on disk — the app will read them via
the "SC1 data path" setting on first run:

```
~/sc1_data/
    StarDat.mpq
    BrooDat.mpq
    patch_rt.mpq
```

Any directory works; the app remembers the path per user in
QSettings.

### 3d. First launch

```bash
# macOS
open build_desktop/simsc_desktop/simsc_desktop.app

# Linux
./build_desktop/simsc_desktop/simsc_desktop
```

On first run, the app forces the **Settings** tab open. Fill in:

1. **SC1 data path** — the directory from step 3c
2. **simsc base URL** — optional, only if using the simsc REST lobby (leave empty for the direct-connect workflow this doc uses)
3. **simsc API key** — same, optional

Save. The Settings tab now shows a green check and Remote Games /
Local Games tabs enable.

---

## 4. Attendee laptop: connect to the remote game

There are two connection flows depending on how you're organizing
the workshop:

### 4a. Direct-connect flow (simple, recommended for small workshops)

You (the organizer) already told each attendee:
- **Server hostname/IP** (e.g. `game.workshop.local` or `10.0.0.5`)
- **Their assigned port pair** (e.g. `6113`/`6114`)
- **Their user spec** (alias + api_key you baked into the `--user`
  flag)

Attendee opens the observer window by pointing their agent code at:

```
ws://<server-host>:<agent-port>/agent
```

and — separately — an observer connection at:

```
ws://<server-host>:<obs-port>/observer
```

The simsc_desktop app does the observer part automatically once you
click "Open Observer" on a game. For the agent connection they use
their bot code (see `agent_readme.md`).

**In simsc_desktop for direct-connect**: the Remote Games tab is
designed around the simsc REST lobby (see 4b). For a raw
direct-connect without that lobby, attendees can use the
`simsc_app` binary instead — a thinner observer-only viewer that
takes a game URL on the command line. Build target is
`OPENBW_BUILD_SIMSC_APP=ON`. Simpler for workshops that don't need
the lobby UI.

### 4b. Lobby flow (if you're running a simsc REST server)

If you've deployed the simsc lobby (the Python/FastAPI service
under `simsc/`), it acts as a game registry: attendees log in with
their user account, see a list of open games, and click "Join". The
lobby knows which openbw_server ports host which games and hands
the desktop client the right URLs.

For a workshop, the flow is:

1. Organizer deploys simsc + openbw_server (see `simsc/deploy/` YAML
   templates for the k8s setup)
2. Organizer creates a user + API key per attendee via the simsc
   web UI (or `simsc/deploy/render.sh` scripting)
3. Attendee configures simsc_desktop with the base URL + API key
4. Attendee opens **Remote Games** tab, sees available games, joins
5. Desktop app opens the observer window automatically

This is more setup up-front but nicer for attendees. Recommend for
workshops of 10+ people.

---

## 5. Attendee laptop: agent bot connection

The observer window is only half the workshop. Attendees run their
own agent code that connects to the same server on the agent port:

```python
# From python_agent/ — see that directory for full examples
import asyncio, websockets, json

async def main():
    uri = "ws://<server-host>:6113/agent"
    async with websockets.connect(uri) as ws:
        # Send greeting with alias + api_key from the --user spec
        await ws.send(json.dumps({
            "type": "greeting",
            "alias": "alice",
            "api_key": "sk-alice-workshop-key",
        }))
        # ... read observations, send commands
```

See `agent_readme.md` and `agent_integration.md` for the full
protocol (JSON control messages, binary sync frames for the tick
stream).

Attendees can write agents in any language that speaks WebSocket +
JSON — `python_agent/` has reference implementations in Python.

---

## 6. Workshop-day runbook (organizer)

Rough sequence for a 2-hour workshop:

1. **T-24h**: build the Docker image, push to whatever registry the
   host uses, or SCP the binary + `original_resources/` if running
   bare-metal. Verify by running one match end-to-end (server up,
   observer connects, agent connects, game ticks).
2. **T-1h**: launch N openbw_server containers on the workshop host,
   one per game. Print a table of `attendee → hostname:port` so
   people can find their game.
3. **T-30m**: attendees have already installed simsc_desktop + placed
   their SC data files (they did this at home). They open the app
   and confirm the Settings tab is green. If not, help them fix
   MPQ paths or Qt install issues.
4. **T-0**: distribute the port table. Attendees connect their
   observer via simsc_desktop and their agent via their own code.
5. **During**: monitor server logs for `SIGSEGV`/`SIGABRT`. If a
   game crashes, restart that container. See "Troubleshooting".
6. **After**: `docker rm -f $(docker ps -q --filter name=game-)` to
   sweep containers.

---

## 7. Troubleshooting

**Attendee sees "Connection refused" from observer:**
- Server host firewall blocks 6113/6114. Open the ports or route
  through your workshop's reverse proxy.
- Wrong port pair. Confirm from the launch script.

**"Authentication failed" on agent connect:**
- The `alias:api_key` sent by the agent doesn't match a `--user`
  spec on the server. Check for typos, extra whitespace, or a
  copy-paste that included the leading/trailing quotes.
- API keys are case-sensitive.

**Observer window renders black:**
- SC1 data path in Settings is wrong or the MPQs are missing.
  Check `~/sc1_data/` contains `StarDat.mpq`, `BrooDat.mpq`,
  `patch_rt.mpq`. Path is remembered in QSettings; check
  `~/Library/Preferences/com.simsc.simsc_desktop.plist` on macOS if
  the setting won't save.

**Game hangs at "Waiting for players":**
- One of the `--user` slots hasn't connected yet. The server waits
  for every slot listed in `--user` before starting the sim.
- If you want to start with N < len(--user) players, either drop
  the extra --user lines or use `--allow-partial-start` (see
  `--help`).

**Server crashes mid-game:**
- The `Dockerfile.server` image has a SIGSEGV handler that dumps a
  stack trace. Check stderr from the container:
  `docker logs game-N`. If it's an openBW bug (not a config
  issue), file it against this repo with the stack trace.

**"Observer close crashes server" symptom:**
- Fixed in commit `e652324`. If you're on an older build, rebuild
  from HEAD.

---

## 8. What's out of scope for this doc

- **HD-mode rendering (SC:R assets)** — SD mode only. HD requires
  a local SC:R install and the sprite_viewer target.
- **The `arena_server` MOBA mode** — planned but not built. See
  the design discussion in the team's notes.
- **The specific values behind the maintainer's private EKS
  deployment at `simsc.agentnumber47.com`** (account ID, Cognito
  pool ARN, ACM cert ARN, etc.) — those live in gitignored
  `aws_account_info/`. Section 10 covers the shape of a simsc
  deployment; fill in your own AWS values.
- **Agent programming** — see `agent_readme.md`,
  `agent_integration.md`, and `python_agent/agents/*.py`.

---

## 9. Quick reference: minimum commands to run one match

**Server side** (single terminal):
```bash
docker build -f Dockerfile.server -t openbw-server:workshop .
docker run --rm -p 6113:6113 -p 6114:6114 openbw-server:workshop \
    --map '/opt/openbw/data/(2)Bottleneck.scm' \
    --user 'alice:sk-alice:player:0' \
    --user 'bob:sk-bob:player:1' \
    --race 0=terran --race 1=zerg \
    --game-speed fastest --any-ws-path
```

**Attendee side** (once, at setup):
```bash
brew install qt@6 cmake                 # macOS
git clone <repo> openbw && cd openbw
cmake -B build_desktop -DOPENBW_BUILD_SIMSC_DESKTOP=ON \
      -DOPENBW_BUILD_OBSERVER=OFF -DOPENBW_BUILD_SERVER=OFF \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build_desktop --target simsc_desktop
```

**Attendee side** (per workshop):
1. Open simsc_desktop, fill Settings tab (SC1 data path)
2. Open observer window pointed at `ws://<server>:6114/observer`
3. Run own agent code against `ws://<server>:6113/agent` with the
   assigned alias + api_key

---

## 10. Optional: deploy the simsc lobby (create-game / list-games)

The `openbw_server` binary is one C++ process that hosts **one
match** with a fixed user list. That's fine for direct-connect
workshops (section 4a). If you want attendees to log into a web
UI, browse open games, and click "join," you need the **simsc**
service too.

### 10a. What simsc is

A FastAPI web app in this same repo, under `simsc/`. It provides:

- **User accounts + API keys** — attendees log in via Cognito, get
  a per-user API key they configure in simsc_desktop
- **REST endpoints** for game lifecycle:
  - `POST /api/games` create a game (draft → pending)
  - `GET /api/games` list games visible to the caller
  - `POST /api/games/{id}/accept` invitee accepts
  - `POST /api/games/{id}/decline` invitee declines (deletes game)
  - `POST /api/games/{id}/cancel` creator cancels
- **Kubernetes launcher** — on accept, simsc creates a Pod running
  the `openbw_server` image with the right `--user` specs
  auto-populated from the game's players, plus an Ingress rule so
  attendees can reach the pod at `wss://<host>/game/<id>/agent`
- **Static SPA** — a browser client for the same REST API (for
  attendees who prefer the web to the Qt desktop app)

Runs as **one container per pod**: Debian 12 + Postgres 16 +
Python 3.12 + uvicorn on port 8080. Postgres data lives on a PVC.

### 10b. Build the simsc-app image

```bash
docker build -f simsc/Dockerfile -t simsc-app:workshop simsc/
```

The build takes ~5 min the first time (installs Postgres, Python,
FastAPI stack). Result: single image runs Postgres + web + alembic
migrations at startup via `simsc/start.sh`.

### 10c. Environment variables simsc needs

Set these before running the container. Read by
`simsc/app/core/config.py`:

| Variable | Purpose |
|---|---|
| `COGNITO_POOL_ID` | Cognito user pool id for auth |
| `COGNITO_CLIENT_ID` | Cognito app client id |
| `COGNITO_DOMAIN` | Cognito hosted-UI domain |
| `COGNITO_REGION` | AWS region of the pool (default `ap-northeast-1`) |
| `SITE_ORIGIN` | Public https URL of the simsc instance (used for CORS + Cognito redirect) |
| `ADMIN_TOKEN` | Random secret enabling admin routes. Generate with `openssl rand -hex 32` |
| `OPENBW_SERVER_IMAGE` | Full ECR path to the openbw_server image simsc should launch (e.g. `<accountid>.dkr.ecr.<region>.amazonaws.com/openbw-server:tag`) |
| `GAMES_NAMESPACE` | k8s namespace to create game Pods in (default `simsc-games`) |
| `GAMES_HOST` | The hostname attendees will use to reach games (used in Ingress rules) |
| `GAMES_ACM_CERT_ARN` | ACM certificate ARN for TLS on the games Ingress |
| `DATABASE_URL` | Postgres URL. Default is the in-pod cluster; override for external DB |

The `SITE_ORIGIN` and `GAMES_HOST` are typically the same
DNS name (e.g. `simsc.workshop.example.com`), served by the same
ALB.

### 10d. Deploy: standalone (no Kubernetes)

Simsc alone can run in a single container against a local
Postgres — useful for testing before deploying to k8s. Won't be
able to launch game pods, but the REST + login flow work:

```bash
docker run --rm -p 8080:8080 \
    -e COGNITO_POOL_ID=us-east-1_XXXXXX \
    -e COGNITO_CLIENT_ID=xxxxxxxxxx \
    -e COGNITO_DOMAIN=your-domain.auth.us-east-1.amazoncognito.com \
    -e SITE_ORIGIN=http://localhost:8080 \
    -e ADMIN_TOKEN=$(openssl rand -hex 32) \
    -e OPENBW_SERVER_IMAGE=openbw-server:workshop \
    simsc-app:workshop
```

Open `http://localhost:8080/` in a browser. The Cognito login redirect will fail without a real pool, so this mode is mostly for testing the shape of the app.

### 10e. Deploy: EKS with k8s

Production shape. Templates live in `simsc/deploy/*.yaml.tmpl` and
`simsc/deploy/render.sh` renders them by substituting env vars:

```
simsc/deploy/
├── 00-secret.yaml.tmpl        Kubernetes Secret with env vars
├── 01-pvc.yaml.tmpl           PersistentVolumeClaim for Postgres data
├── 02-deployment.yaml.tmpl    Deployment: 1 replica of simsc-app
├── 03-ingress-public.yaml.tmpl  ALB Ingress, public paths (no auth)
├── 04-ingress-cognito.yaml.tmpl ALB Ingress, Cognito-gated paths
├── 05-rbac.yaml.tmpl          Role/RoleBinding — simsc's k8s access to launch games
└── render.sh                  Substitutes env vars into all templates
```

Required env vars for `render.sh` (from `render.sh` itself):

```
AWS_REGION
AWS_ACCOUNT_ID
ECR_IMAGE              # simsc-app image URI in ECR
IMAGE_TAG              # tag to deploy
OPENBW_SERVER_IMAGE    # openbw_server image URI simsc will launch
K8S_NAMESPACE
COGNITO_POOL_ARN
COGNITO_CLIENT_ID
COGNITO_DOMAIN
COGNITO_REGION
ACM_CERT_ARN
SITE_ORIGIN
SITE_HOST
ADMIN_TOKEN_BASE64
```

Deploy flow, once those are exported in the shell:

```bash
cd simsc/deploy
./render.sh                        # creates *.yaml from *.yaml.tmpl
kubectl apply -f 00-secret.yaml
kubectl apply -f 01-pvc.yaml
kubectl apply -f 05-rbac.yaml
kubectl apply -f 02-deployment.yaml
kubectl apply -f 03-ingress-public.yaml
kubectl apply -f 04-ingress-cognito.yaml
```

The rendered `*.yaml` files are gitignored — they contain account
IDs and are meant to be regenerated per environment.

Post-deploy checks:

```bash
kubectl -n <ns> get pods                        # simsc-app pod Running
kubectl -n <ns> logs deploy/simsc-app -f        # migrations + uvicorn boot
curl https://<SITE_HOST>/api/health             # 200
```

Then open `https://<SITE_HOST>/` in a browser, log in with a
Cognito user, and try creating a game.

### 10f. What simsc automates that direct-connect doesn't

Once simsc is running, an attendee's workflow becomes:

1. Log in at `https://<SITE_HOST>/`, copy their API key from the profile page (revealed exactly once)
2. Paste the API key + `SITE_HOST` into simsc_desktop's Settings tab
3. Open the **Remote Games** tab → see games → click "Create" or "Join"
4. simsc_desktop's UI shows the game state (pending → accepted → running)
5. Once the game is running, "Open Observer" launches the observer window automatically pointed at the k8s-ingress URL

No hostname/port table to hand out. No `--user` inline specs. simsc
manages user identity, game pods, and observer URLs for you.

### 10g. Prerequisites simsc pulls in

Deploying simsc is a real project vs. direct-connect. You need:

- **AWS account** with EKS cluster (single cluster is fine)
- **Cognito user pool** with app client + hosted UI domain (attendees log in through this)
- **ACM certificate** for your workshop hostname
- **Route53 (or equivalent DNS)** pointing the hostname at the ALB
- **ECR** for the two images (simsc-app, openbw-server)
- **AWS Load Balancer Controller** installed in the cluster (creates ALBs from Ingress resources)
- **StorageClass** with dynamic provisioning (for the Postgres PVC)

If you don't have these, direct-connect (section 4a) is far
cheaper for a one-off workshop. simsc pays for itself when you're
running many workshops or want persistent user identity across
sessions.

### 10h. What's still deliberately out of scope

The specific hostname / account ID / Cognito pool / ACM ARN values
for the maintainer's existing deployment
(`simsc.agentnumber47.com`) live in the gitignored
`aws_account_info/` directory. They're the organizer's own
infrastructure and aren't reproducible from this doc alone. If
you're standing up a **new** simsc instance in a **new** AWS
account, use this section as a checklist and fill in your own
values.
