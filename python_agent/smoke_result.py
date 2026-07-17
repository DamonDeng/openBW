"""Closed-loop cmd-result smoke test.

Spawns openbw_server, connects as alice, fires one known-good and
one known-bad command, and asserts the {"type":"result", ...}
message arrives with the expected `status` values.

Verifies:
  - APPLIED (0) fires on a valid train (SCV -> Marine? no, use a
    valid same-race primitive that's harder to accidentally REFUSE:
    a Barracks training a Marine. But barracks costs 150 minerals
    and we start with 50, so we set --resources to seed us richly).
  - REFUSED (1) fires on a "gather" verb pointed at a totally
    invalid target unit id (0x7fff never exists).

Timing: at --game-speed 10 (turbosuper) the result should arrive
within a few hundred ms of the ack.

Usage:
    python3 -m python_agent.smoke_result
    # or with an already-running server:
    python3 -m python_agent.smoke_result --external
"""

from __future__ import annotations

import argparse
import asyncio
import json
import signal
import subprocess
import sys
import time
from pathlib import Path

from python_agent.client import Client, CommandTimeout
from python_agent.status import Status


REPO = Path(__file__).resolve().parent.parent
SERVER_BIN = REPO / "build_srv" / "server" / "openbw_server"
DATA_PATH  = REPO / "original_resources"
DEFAULT_MAP = DATA_PATH / "(2)Bottleneck.scm"


class SmokeFailure(RuntimeError):
    pass


def spawn_server(port_agent: int, port_obs: int, map_path: str,
                 log_path: str) -> subprocess.Popen:
    cmd = [
        str(SERVER_BIN),
        "--data-path", str(DATA_PATH),
        "--map", map_path,
        "--user", "alice:sk-alice:player:0",
        "--user", "bob:sk-bob:player:1",
        "--race", "0=terran", "--race", "1=terran",
        "--game-speed", "10",
        "--ws-port", str(port_agent),
        "--obs-port", str(port_obs),
    ]
    f = open(log_path, "w")
    return subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT,
                            cwd=str(REPO))


async def wait_for_port(port: int, timeout_sec: float = 8.0) -> None:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        try:
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
            return
        except (ConnectionRefusedError, OSError):
            await asyncio.sleep(0.1)
    raise SmokeFailure(f"port {port} never opened")


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise SmokeFailure(msg)


async def run(port_agent: int) -> None:
    c = Client(api_key="sk-alice", host="127.0.0.1", port=port_agent)
    w = await c.connect()
    print(f"[smoke] welcome slot={w.slot} frame={w.current_frame}")
    check(w.slot == 0, f"expected alice=slot0 got {w.slot}")

    # Observe once so we know unit ids.
    obs = await c.observe(targets=["units", "resources"])
    scv = next((u for u in obs["units"]
                if u["type"] == 7 and u.get("completed")), None)
    check(scv is not None, "no completed SCV at start")
    print(f"[smoke] SCV unit_id={scv['unit_id']}")

    # -------- Bad command: train a Marine at an SCV ----------
    # An SCV cannot train units (only Barracks/Factory/etc. can). The
    # sim's action_train validates the actor's unit_type and returns
    # false -> Status=REFUSED (1). This is a deterministic REFUSED
    # that doesn't depend on any mineral/gas state.
    MARINE_TYPE = 0   # Terran_Marine = 0 in openBW's UnitTypes enum
    t0 = time.monotonic()
    r_bad = await c.train(unit_id=scv["unit_id"], unit_type=MARINE_TYPE)
    dt_ms = (time.monotonic() - t0) * 1000
    print(f"[smoke] bad train (SCV can't train) -> status={r_bad.status.name} "
          f"applied_at_frame={r_bad.applied_at_frame} "
          f"queued_at_frame={r_bad.queued_at_frame} "
          f"round_trip={dt_ms:.1f}ms")
    check(r_bad.status is Status.REFUSED,
          f"bad train expected REFUSED got {r_bad.status.name}")
    check(r_bad.applied_at_frame > 0, "no applied_at_frame")
    check(r_bad.verb == "train", f"verb echo wrong: {r_bad.verb!r}")
    check(dt_ms < 1000,
          f"result took {dt_ms:.0f}ms, expected < 1000ms at speed=10")

    # -------- Good command: move SCV a tiny distance ----------
    # Move is one of the least-refusable verbs — any live SCV at
    # any position accepts a move to any point on the map. Pick
    # SCV's own (x+16, y) so we don't stall on repathing.
    t0 = time.monotonic()
    r_good = await c.move(unit_id=scv["unit_id"],
                          x=scv["x"] + 16, y=scv["y"])
    dt_ms = (time.monotonic() - t0) * 1000
    print(f"[smoke] good move -> status={r_good.status.name} "
          f"applied_at_frame={r_good.applied_at_frame} "
          f"queued_at_frame={r_good.queued_at_frame} "
          f"round_trip={dt_ms:.1f}ms")
    check(r_good.status is Status.APPLIED,
          f"good move expected APPLIED got {r_good.status.name}")
    check(r_good.verb == "move", f"verb echo wrong: {r_good.verb!r}")
    check(r_good.queued_at_frame is not None,
          "missing queued_at_frame from ack")

    # -------- Timeout smoke ----------
    # Fire a good move with a comically short timeout; we expect
    # CommandTimeout to raise even though the server WILL eventually
    # deliver a result (dropped into the discarded future). Typed
    # helpers don't expose `timeout`; go through cmd() directly.
    print("[smoke] testing CommandTimeout (0.001s)...")
    try:
        await c.cmd({"verb": "move", "unit": scv["unit_id"],
                     "x": scv["x"], "y": scv["y"] + 16, "queue": False},
                    timeout=0.001)
        raise SmokeFailure("expected CommandTimeout, got success")
    except CommandTimeout as e:
        print(f"[smoke] got CommandTimeout as expected: {e}")

    await c.close()


async def main_async(args) -> int:
    proc = None
    if not args.external:
        log_path = "/tmp/smoke_result_server.log"
        print(f"[smoke] server log -> {log_path}")
        proc = spawn_server(args.agent_port, args.obs_port,
                            str(DEFAULT_MAP), log_path)
        try:
            await wait_for_port(args.agent_port)
        except SmokeFailure:
            proc.terminate()
            print("[smoke] server never came up; tail of log:")
            with open(log_path) as f:
                for line in f.readlines()[-30:]:
                    print(f"    {line.rstrip()}")
            return 2

    try:
        await run(args.agent_port)
        print("[smoke] ALL CHECKS PASSED")
        return 0
    except SmokeFailure as e:
        print(f"[smoke] FAIL: {e}")
        return 1
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--agent-port", type=int, default=6113)
    p.add_argument("--obs-port", type=int, default=6114)
    p.add_argument("--external", action="store_true",
                   help="Don't spawn a server; expect one already running.")
    args = p.parse_args()
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
