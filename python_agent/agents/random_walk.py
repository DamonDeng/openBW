"""Random-walk agent: pick a random idle worker each tick, move it to a
random map position.

Runs forever. Intended as (a) a template for attendees and (b) the
smallest end-to-end test that exercises observe + cmd.

Usage:
    python3 -m python_agent.agents.random_walk <api_key>
        [--host 127.0.0.1] [--port 6113] [--interval-sec 1.5]
"""

from __future__ import annotations

import argparse
import asyncio
import random
import sys

from python_agent.client import Client
from python_agent.enums import WORKER_TYPES, IDLE_ORDERS, unit_type_name, order_name


async def main(api_key: str, host: str, port: int, interval_sec: float) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        print(f"[agent] connected as slot={c.welcome.slot} "
              f"at frame={c.welcome.current_frame}")

        map_info = (await c.observe(targets=["map_info"]))["map_info"]
        w, h = map_info["width"], map_info["height"]
        print(f"[agent] map={w}x{h} pixels")

        rng = random.Random()
        while True:
            obs = await c.observe(targets=["units", "resources"])
            workers = [u for u in obs["units"]
                       if u["type"] in WORKER_TYPES
                       and u["order"] in IDLE_ORDERS]
            print(f"[agent] frame={obs['current_frame']} "
                  f"minerals={obs['resources']['minerals']} "
                  f"units={len(obs['units'])} idle_workers={len(workers)}")
            if workers:
                w_unit = rng.choice(workers)
                tx = rng.randrange(64, w - 64)
                ty = rng.randrange(64, h - 64)
                try:
                    ack = await c.move(unit_id=w_unit["unit_id"], x=tx, y=ty)
                    print(f"[agent]  moved {unit_type_name(w_unit['type'])} "
                          f"{w_unit['unit_id']} -> ({tx},{ty}) "
                          f"applied@frame={ack['queued_at_frame']}")
                except Exception as e:
                    print(f"[agent]  cmd error: {e}")
            await asyncio.sleep(interval_sec)


def entrypoint() -> None:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.agents.random_walk")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=1.5)
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port, args.interval_sec))
    except KeyboardInterrupt:
        print("\n[agent] stopped")


if __name__ == "__main__":
    entrypoint()
