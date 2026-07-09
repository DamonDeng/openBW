"""End-to-end smoke test for the openBW agent stack.

Spawns openbw_server as a subprocess, connects a client as alice,
runs a short scripted scenario, and asserts the server behaves. Exits
non-zero on failure with a readable error.

Intended as (a) the fast regression check we run after every server
change, and (b) a reference for how a workshop attendee might write
their own automated tests.

Usage:
    python3 -m python_agent.smoke_test [--server-bin path] [--map path]
        [--users path] [--data-path path] [--keep-server]

Defaults resolve to the repo's built-in test setup.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

from python_agent.client import Client
from python_agent.enums import (
    WORKER_TYPES, IDLE_ORDERS, unit_type_name, order_name,
)


REPO = Path(__file__).resolve().parent.parent
DEFAULTS = dict(
    server_bin=str(REPO / "build_srv" / "server" / "openbw_server"),
    map=str(REPO / "original_resources" / "(2)Bottleneck.scm"),
    users=str(REPO / "test_resources" / "users.json"),
    data_path=str(REPO / "original_resources"),
    # alice's key from the built-in test users file. Read from disk so
    # this stays correct when users.json is regenerated.
    users_key_alias="alice",
)


class SmokeFailure(RuntimeError):
    pass


def load_api_key(users_path: str, alias: str) -> str:
    with open(users_path) as f:
        users = json.load(f)["users"]
    for u in users:
        if u["alias"] == alias:
            return u["api_key"]
    raise SmokeFailure(f"user {alias!r} not found in {users_path}")


def spawn_server(args) -> subprocess.Popen:
    cmd = [
        args.server_bin,
        "--map", args.map,
        "--data-path", args.data_path,
        "--users", args.users,
    ]
    print(f"[smoke] spawning: {' '.join(cmd)}")
    # Merge stderr into stdout so we can dump both on failure.
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    return proc


async def wait_for_port(port: int, timeout_sec: float = 5.0) -> None:
    """Poll until the server's WS port accepts connections."""
    deadline = time.monotonic() + timeout_sec
    last_err: Exception | None = None
    while time.monotonic() < deadline:
        try:
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
            return
        except (ConnectionRefusedError, OSError) as e:
            last_err = e
            await asyncio.sleep(0.1)
    raise SmokeFailure(f"port {port} never opened: {last_err}")


# ---------------- Assertions ----------------

def check(cond: bool, msg: str) -> None:
    if not cond:
        raise SmokeFailure(msg)


def check_welcome(w) -> None:
    check(w is not None, "no welcome received")
    check(w.slot == 0, f"expected alice=slot 0, got slot={w.slot}")
    check(w.current_frame >= 0, f"weird frame: {w.current_frame}")


def check_observation(obs: dict) -> None:
    check(obs["type"] == "observation", f"bad type: {obs['type']!r}")
    check(obs["slot"] == 0, f"slot mismatch: {obs['slot']}")
    check("resources" in obs, "no resources in observation")
    check("units" in obs, "no units in observation")
    check(len(obs["units"]) > 0, "no starting units??")
    # Every unit should have the required fields.
    for u in obs["units"]:
        for k in ("unit_id", "type", "x", "y", "hp", "hp_max", "order"):
            check(k in u, f"unit missing {k}: {u}")


# ---------------- Scenarios ----------------

async def scenario_observe_move_verify(c: Client) -> None:
    """Observe -> pick a worker -> move it -> observe again -> assert moved."""
    print("[smoke] scenario: observe + move + verify")
    obs = await c.observe()
    check_observation(obs)

    workers = [u for u in obs["units"]
               if u["type"] in WORKER_TYPES and u["order"] in IDLE_ORDERS]
    check(len(workers) > 0, f"no idle workers found in slot 0's units: "
          f"{[(unit_type_name(u['type']), order_name(u['order'])) for u in obs['units']]}")
    w = workers[0]
    start_x, start_y = w["x"], w["y"]
    target_x, target_y = 2048, 2048
    print(f"[smoke]  moving {unit_type_name(w['type'])} {w['unit_id']} "
          f"from ({start_x},{start_y}) toward ({target_x},{target_y})")

    ack = await c.move(unit_id=w["unit_id"], x=target_x, y=target_y)
    check(ack["type"] == "ack", f"bad ack: {ack}")
    check(ack["queued_at_frame"] >= obs["current_frame"],
          f"queued_at_frame in the past: {ack}")

    # Give the sim ~2 seconds (48 frames) to actually move the unit.
    await asyncio.sleep(2.0)
    obs2 = await c.observe()
    # Find the same unit and check position changed OR order changed to Move.
    same = next((u for u in obs2["units"] if u["unit_id"] == w["unit_id"]), None)
    check(same is not None, "unit vanished after move")
    moved = (same["x"], same["y"]) != (start_x, start_y)
    ordered = order_name(same["order"]) in ("Move", "MoveToAttack", "AttackMove")
    check(moved or ordered,
          f"unit didn't respond: was ({start_x},{start_y}) order=Guard/etc, "
          f"now ({same['x']},{same['y']}) order={order_name(same['order'])}")
    print(f"[smoke]  unit now at ({same['x']},{same['y']}) "
          f"order={order_name(same['order'])} -- OK")


async def scenario_neutrals_visible(c: Client) -> None:
    """We should see nearby mineral fields in the neutrals list."""
    print("[smoke] scenario: neutrals visible")
    obs = await c.observe(targets=["neutrals"])
    minerals = [n for n in obs.get("neutrals", [])
                if n["type"] in (176, 177, 178)]
    check(len(minerals) > 0, "no mineral fields visible from starting base")
    print(f"[smoke]  {len(minerals)} mineral fields visible -- OK")


async def scenario_gather_verb(c: Client) -> None:
    """Send a worker to gather; after a few seconds, minerals should
    start climbing."""
    print("[smoke] scenario: gather verb actually mines")
    obs = await c.observe()
    starting_minerals = obs["resources"]["minerals"]
    workers = [u for u in obs["units"]
               if u["type"] in WORKER_TYPES and u["order"] in IDLE_ORDERS]
    check(len(workers) > 0, "no idle workers to gather with")

    # Find a mineral to point them at.
    obs_n = await c.observe(targets=["neutrals"])
    minerals = [n for n in obs_n.get("neutrals", [])
                if n["type"] in (176, 177, 178)]
    check(len(minerals) > 0, "no mineral to gather")
    m = minerals[0]

    for w in workers[:2]:  # send two workers
        await c.gather(unit_id=w["unit_id"], target_unit=m["unit_id"])

    # Wait long enough for a full mining trip. On fastest ~ 5s is
    # marginal; give it 10.
    await asyncio.sleep(10.0)
    obs2 = await c.observe()
    delta = obs2["resources"]["minerals"] - starting_minerals
    check(delta >= 8,
          f"minerals only rose by {delta} after 10s of gather; "
          f"expected >= 8 (one mining trip = 8)")
    print(f"[smoke]  minerals +{delta} after 10s of gather -- OK")


async def scenario_error_on_bad_verb(c: Client) -> None:
    """The server should reply with an error message for unknown verbs."""
    print("[smoke] scenario: server rejects invalid verb")
    try:
        await c.cmd({"verb": "nonsense_verb"})
    except Exception as e:
        # Our Client raises AgentError for type=error responses.
        from python_agent.client import AgentError
        check(isinstance(e, AgentError),
              f"expected AgentError, got {type(e).__name__}: {e}")
        print(f"[smoke]  got expected error: {e} -- OK")
        return
    raise SmokeFailure("bad verb did not raise")


# ---------------- Main ----------------

async def run_scenarios(api_key: str) -> None:
    print("[smoke] connecting to ws://127.0.0.1:6113 ...")
    async with Client(api_key=api_key) as c:
        check_welcome(c.welcome)
        print(f"[smoke]  welcomed as slot={c.welcome.slot} frame={c.welcome.current_frame}")

        await scenario_neutrals_visible(c)
        await scenario_error_on_bad_verb(c)
        await scenario_observe_move_verify(c)
        await scenario_gather_verb(c)


def main() -> int:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.smoke_test")
    p.add_argument("--server-bin", default=DEFAULTS["server_bin"])
    p.add_argument("--map", default=DEFAULTS["map"])
    p.add_argument("--users", default=DEFAULTS["users"])
    p.add_argument("--data-path", default=DEFAULTS["data_path"])
    p.add_argument("--users-key-alias", default=DEFAULTS["users_key_alias"])
    p.add_argument("--keep-server", action="store_true",
                   help="don't kill the server on exit (for debugging)")
    args = p.parse_args()

    # Fail fast if inputs are missing.
    for name in ("server_bin", "map", "users", "data_path"):
        path = getattr(args, name if name != "data_path" else "data_path")
        if not Path(path).exists():
            print(f"[smoke] error: {name}={path} does not exist", file=sys.stderr)
            return 2

    api_key = load_api_key(args.users, args.users_key_alias)
    proc = spawn_server(args)
    try:
        # Wait for both ports.
        asyncio.run(wait_for_port(6112, timeout_sec=5.0))
        asyncio.run(wait_for_port(6113, timeout_sec=5.0))
        # Give the sim a moment to actually reach start_game_impl.
        time.sleep(1.5)

        try:
            asyncio.run(run_scenarios(api_key))
        except SmokeFailure as e:
            print(f"[smoke] FAIL: {e}", file=sys.stderr)
            print("\n[smoke] --- server output ---", file=sys.stderr)
            try:
                proc.stdout and print(proc.stdout.read(), file=sys.stderr)
            except Exception:
                pass
            return 1
        except Exception as e:
            print(f"[smoke] FAIL: unexpected: {type(e).__name__}: {e}",
                  file=sys.stderr)
            return 1

        print("[smoke] all scenarios PASSED")
        return 0
    finally:
        if args.keep_server:
            print(f"[smoke] leaving server running (pid={proc.pid})")
        else:
            try:
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    sys.exit(main())
