"""Miner agent: send every idle worker to gather from the nearest
mineral field.

Uses the dedicated "gather" verb (Orders::Harvest1 under the hood) so
the sim actually starts a mining cycle. Workers already busy mining
are left alone.

Usage:
    python3 -m python_agent.agents.miner <api_key> [--interval-sec S]
"""

from __future__ import annotations

import argparse
import asyncio

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name, order_name, ORDERS_BY_NAME,
)
from python_agent.helpers import (
    workers, mineral_fields, nearest, IDLE_ORDERS,
)


# Orders that mean "already busy on a mining trip". Don't disturb them.
MINING_ORDERS: set[int] = {
    ORDERS_BY_NAME["Harvest1"],
    ORDERS_BY_NAME["Harvest2"],
    ORDERS_BY_NAME["MoveToMinerals"],
    ORDERS_BY_NAME["WaitForMinerals"],
    ORDERS_BY_NAME["MiningMinerals"],
    ORDERS_BY_NAME["ReturnMinerals"],
    ORDERS_BY_NAME["MoveToGas"],
    ORDERS_BY_NAME["WaitForGas"],
    ORDERS_BY_NAME["HarvestGas"],
    ORDERS_BY_NAME["ReturnGas"],
}


async def run(c: Client, interval_sec: float) -> None:
    print(f"[miner] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    while True:
        obs = await c.observe(targets=["units", "resources", "enemies"])
        wu = workers(obs["units"])
        mfs = mineral_fields(obs.get("neutrals", []))
        idle = [u for u in wu if u["order"] in IDLE_ORDERS]
        mining_now = sum(1 for u in wu if u["order"] in MINING_ORDERS)

        print(f"[miner] frame={obs['current_frame']} "
              f"workers={len(wu)} mining={mining_now} idle={len(idle)} "
              f"minerals={obs['resources']['minerals']}")

        if not mfs:
            # Vision hasn't caught up (rare after first tick).
            await asyncio.sleep(interval_sec)
            continue

        for w in idle:
            m = nearest(w, mfs)
            if m is None:
                continue
            try:
                ack = await c.gather(unit_id=w["unit_id"],
                                     target_unit=m["unit_id"])
                print(f"[miner]  {unit_type_name(w['type'])} "
                      f"{w['unit_id']} -> mineral {m['unit_id']} "
                      f"@frame={ack['queued_at_frame']}")
            except Exception as e:
                print(f"[miner]  cmd error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int, interval_sec: float) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec)


def entrypoint() -> None:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.agents.miner")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=2.0)
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port, args.interval_sec))
    except KeyboardInterrupt:
        print("\n[miner] stopped")


if __name__ == "__main__":
    entrypoint()
