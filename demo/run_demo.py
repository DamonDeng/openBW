#!/usr/bin/env python3
"""Local demo launcher: one server + two agents + three observer windows.

Layout:
  - openbw_server on ports 6113 (agent WS) and 6114 (observer WS)
  - two agents, latest per resolved race, one per slot
  - three simsc_app windows: slot-0 perspective, slot-1 perspective,
    admin no-fog spectator

Ctrl-C once tears down all six processes in reverse spawn order and
exits with code 130. Per-process stderr/stdout land in demo/logs/ for
post-mortem.

Run from repo root:
    python3 demo/run_demo.py

See demo/README.md for the full option surface.
"""

import argparse
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

# Repo root = parent of this file's directory.
REPO_ROOT = Path(__file__).resolve().parent.parent

SERVER_BIN  = REPO_ROOT / "build_srv" / "server"    / "openbw_server"
SIMSC_BIN   = REPO_ROOT / "build_qt"  / "simsc_app" / "simsc_app"
DATA_PATH   = REPO_ROOT / "original_resources"
DEFAULT_MAP = DATA_PATH / "(2)Bottleneck.scm"

# Newest per-race agents. Bump when a new agent version ships.
AGENT_MODULES = {
    "zerg":    "python_agent.agents.z_agent_v5",
    "terran":  "python_agent.agents.t_agent_v6_7",
    "protoss": "python_agent.agents.p_agent_v4",
}

RACES = ("zerg", "terran", "protoss")

# Demo-only API keys. Not for production — hard-coded so agents +
# observers can find each other on 127.0.0.1 without a users.json.
KEYS = {
    "alice": "sk-alice",   # slot 0 player
    "bob":   "sk-bob",     # slot 1 player
    "admin": "sk-admin",   # no-fog spectator
}


def log(msg):
    print(f"[demo] {msg}", flush=True)


def parse_race_arg(v):
    # accepts "N=NAME"
    if "=" not in v:
        raise argparse.ArgumentTypeError(f"--race must be N=RACE, got {v!r}")
    slot_s, name = v.split("=", 1)
    slot = int(slot_s)
    if slot not in (0, 1):
        raise argparse.ArgumentTypeError(f"--race slot must be 0 or 1, got {slot}")
    name = name.strip().lower()
    if name not in RACES:
        raise argparse.ArgumentTypeError(
            f"--race value must be one of {RACES}, got {name!r}")
    return slot, name


def parse_args():
    p = argparse.ArgumentParser(
        description="Launch a local openBW demo: server + agents + 3 observers.")
    p.add_argument("--map", default=str(DEFAULT_MAP),
                   help=f"Map .scm/.scx path (default: {DEFAULT_MAP.name}).")
    p.add_argument("--race", action="append", type=parse_race_arg, default=[],
                   help="N=RACE, repeat per slot. Missing slots get a random race.")
    p.add_argument("--game-speed", default="10",
                   help="ms/frame or BW name (default: 10 = turbosuper).")
    p.add_argument("--agent-port", type=int, default=6113,
                   help="Agent WebSocket port (default: 6113).")
    p.add_argument("--obs-port", type=int, default=6114,
                   help="Observer WebSocket port (default: 6114).")
    p.add_argument("--log-dir", default=str(REPO_ROOT / "demo" / "logs"),
                   help="Per-process log dir (default: demo/logs).")
    return p.parse_args()


def port_open(port):
    """True iff something is already listening on 127.0.0.1:port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.2)
        try:
            s.connect(("127.0.0.1", port))
            return True
        except (ConnectionRefusedError, socket.timeout, OSError):
            return False


def preflight(args):
    problems = []
    if not SERVER_BIN.exists():
        problems.append(f"server binary missing: {SERVER_BIN}"
                        f"\n  build with: cmake --build build_srv --target openbw_server")
    if not SIMSC_BIN.exists():
        problems.append(f"simsc_app binary missing: {SIMSC_BIN}"
                        f"\n  build with: cmake --build build_qt --target simsc_app")
    map_path = Path(args.map)
    if not map_path.is_absolute():
        map_path = REPO_ROOT / map_path
    if not map_path.exists():
        problems.append(f"map not found: {map_path}")
    for name, port in [("agent", args.agent_port), ("observer", args.obs_port)]:
        if port_open(port):
            problems.append(
                f"port {port} ({name}) is already in use\n"
                f"  hint: pkill -f openbw_server && pkill -f simsc_app")
    if not shutil.which("python3"):
        problems.append("python3 not on PATH")
    if problems:
        for p in problems:
            log("preflight FAIL: " + p)
        sys.exit(2)


def resolve_races(explicit):
    """Merge explicit --race pairs into a [race0, race1] list; random-fill."""
    resolved = [None, None]
    sources  = ["random", "random"]
    for slot, name in explicit:
        resolved[slot] = name
        sources[slot]  = "explicit"
    for i in range(2):
        if resolved[i] is None:
            resolved[i] = random.choice(RACES)
    for i in range(2):
        log(f"slot {i} = {resolved[i]} ({sources[i]})")
    return resolved


def wait_for_server_ready(server_log_path, timeout_sec=10):
    """Poll the server log for the 'observer WS listening' line."""
    deadline = time.monotonic() + timeout_sec
    needle = "observer WS listening"
    while time.monotonic() < deadline:
        try:
            with open(server_log_path) as f:
                if needle in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.15)
    return False


def spawn(cmd, log_path, env=None):
    """Popen wrapper — everything goes to log_path, no shell."""
    f = open(log_path, "w")
    return subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT,
                            cwd=REPO_ROOT, env=env)


def terminate_all(procs, label_by_pid):
    """Reverse-order SIGTERM, 3s wait each, SIGKILL survivors."""
    for p in reversed(procs):
        if p.poll() is not None:
            continue
        label = label_by_pid.get(p.pid, str(p.pid))
        log(f"terminate {label} (pid {p.pid})")
        try:
            p.terminate()
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 3.0
    for p in reversed(procs):
        remaining = max(0.0, deadline - time.monotonic())
        try:
            p.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            label = label_by_pid.get(p.pid, str(p.pid))
            log(f"kill {label} (pid {p.pid})")
            try:
                p.kill()
            except ProcessLookupError:
                pass


def main():
    args = parse_args()
    preflight(args)

    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)

    races = resolve_races(args.race)
    map_path = args.map
    if not Path(map_path).is_absolute():
        map_path = str(REPO_ROOT / map_path)

    procs = []
    label_by_pid = {}

    def add(p, label):
        procs.append(p)
        label_by_pid[p.pid] = label

    # --- server -----------------------------------------------------------
    server_log = log_dir / "server.log"
    server_cmd = [
        str(SERVER_BIN),
        "--data-path", str(DATA_PATH),
        "--map", map_path,
        "--user", f"alice:{KEYS['alice']}:player:0",
        "--user", f"bob:{KEYS['bob']}:player:1",
        "--user", f"admin:{KEYS['admin']}:admin",
        "--race", f"0={races[0]}",
        "--race", f"1={races[1]}",
        "--game-speed", str(args.game_speed),
        "--obs-port", str(args.obs_port),
        "--ws-port",  str(args.agent_port),
    ]
    log(f"starting server -> {server_log}")
    server_proc = spawn(server_cmd, server_log)
    add(server_proc, "server")

    if not wait_for_server_ready(server_log):
        log("server did not reach 'observer WS listening' within 10s; aborting")
        terminate_all(procs, label_by_pid)
        sys.exit(3)
    log("server ready")

    # --- observers (slot0 player, slot1 player, admin no-fog) -------------
    observer_specs = [
        ("simsc_slot0", KEYS["alice"], f"slot 0 (alice, {races[0]})"),
        ("simsc_slot1", KEYS["bob"],   f"slot 1 (bob,   {races[1]})"),
        ("simsc_admin", KEYS["admin"], "admin (no fog)"),
    ]
    obs_url = f"ws://127.0.0.1:{args.obs_port}/observer"
    for logname, key, label in observer_specs:
        cmd = [
            str(SIMSC_BIN),
            "--data-path", str(DATA_PATH),
            "--map", map_path,
            "--url", obs_url,
            "--api-key", key,
            "--race", f"0={races[0]}",
            "--race", f"1={races[1]}",
        ]
        log(f"starting {label} -> {logname}.log")
        p = spawn(cmd, log_dir / f"{logname}.log")
        add(p, logname)
        # 1.5s spacing — three WS handshakes back-to-back races
        # the server's observer accept path and one gets kicked
        # (empirically: 0.5s is too tight, 1.5s is stable). Also
        # gives the window manager space to position each Qt
        # window separately instead of stacking them.
        time.sleep(1.5)

    # --- agents (slot0 first, then slot1) ---------------------------------
    agent_env = os.environ.copy()
    # Force PYTHONPATH so `python3 -m python_agent.agents.*` resolves from
    # the repo root regardless of what the user set globally.
    agent_env["PYTHONPATH"] = str(REPO_ROOT) + os.pathsep + agent_env.get("PYTHONPATH", "")

    agent_specs = [
        (0, KEYS["alice"], races[0]),
        (1, KEYS["bob"],   races[1]),
    ]
    for slot, key, race in agent_specs:
        module = AGENT_MODULES[race]
        cmd = [
            sys.executable, "-u", "-m", module, key,
            "--host", "127.0.0.1",
            "--port", str(args.agent_port),
        ]
        logname = f"agent_slot{slot}"
        log(f"starting agent slot {slot} ({race}, {module}) -> {logname}.log")
        p = spawn(cmd, log_dir / f"{logname}.log", env=agent_env)
        add(p, logname)

    log("all six processes up; Ctrl-C to stop.")
    log(f"logs at {log_dir}")

    # --- wait -------------------------------------------------------------
    interrupted = {"n": 0}

    def on_sigint(_signum, _frame):
        interrupted["n"] += 1
        if interrupted["n"] == 1:
            log("SIGINT received; tearing down...")
        else:
            log("second SIGINT; aborting immediately")
            for p in procs:
                try:
                    p.kill()
                except ProcessLookupError:
                    pass
            sys.exit(130)

    signal.signal(signal.SIGINT, on_sigint)

    try:
        # Anchor on the server. If it dies (crash or manual kill) we
        # tear everything down. Also break out on any Ctrl-C.
        while True:
            if interrupted["n"] > 0:
                break
            rc = server_proc.poll()
            if rc is not None:
                log(f"server exited (rc={rc}); tearing down remaining processes")
                break
            time.sleep(0.5)
    finally:
        terminate_all(procs, label_by_pid)

    exit_code = 130 if interrupted["n"] else (server_proc.returncode or 0)
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
