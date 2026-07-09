"""Miner agent: mine minerals + gas.

Idle workers are sent to the nearest mineral field via the gather verb
(Orders::Harvest1 under the hood). When a completed refinery /
assimilator / extractor is visible, we top up to ~3 workers per gas
structure by re-tasking already-mining workers off minerals -- gas
is worth more per trip than the marginal 4th miner on a patch.

Usage:
    python3 -m python_agent.agents.miner <api_key> [--gas-workers N]
"""

from __future__ import annotations

import argparse
import asyncio

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name, ORDERS_BY_NAME,
)
from python_agent.helpers import (
    workers, mineral_fields, nearest, IDLE_ORDERS, own_refineries,
)


# Orders that mean "already busy on a mining trip". Don't disturb them.
GAS_ORDERS: set[int] = {
    ORDERS_BY_NAME["MoveToGas"],
    ORDERS_BY_NAME["WaitForGas"],
    ORDERS_BY_NAME["HarvestGas"],
    ORDERS_BY_NAME["ReturnGas"],
}

MINERAL_ORDERS: set[int] = {
    ORDERS_BY_NAME["Harvest1"],
    ORDERS_BY_NAME["Harvest2"],
    ORDERS_BY_NAME["MoveToMinerals"],
    ORDERS_BY_NAME["WaitForMinerals"],
    ORDERS_BY_NAME["MiningMinerals"],
    ORDERS_BY_NAME["ReturnMinerals"],
}

MINING_ORDERS = GAS_ORDERS | MINERAL_ORDERS


async def run(c: Client, interval_sec: float, target_gas_workers: int) -> None:
    print(f"[miner] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    while True:
        obs = await c.observe(targets=["units", "resources", "enemies"])
        wu = workers(obs["units"])
        mfs = mineral_fields(obs.get("neutrals", []))
        refineries = own_refineries(obs["units"])
        idle = [u for u in wu if u["order"] in IDLE_ORDERS]
        on_gas = sum(1 for u in wu if u["order"] in GAS_ORDERS)
        on_min = sum(1 for u in wu if u["order"] in MINERAL_ORDERS)

        r = obs["resources"]
        print(f"[miner] frame={obs['current_frame']} workers={len(wu)} "
              f"idle={len(idle)} on_min={on_min} on_gas={on_gas} "
              f"refineries={len(refineries)} min={r['minerals']} gas={r['gas']}")

        # ---- 1. Fill gas assignments first (more valuable per trip). ----
        gas_needed = len(refineries) * target_gas_workers - on_gas
        if gas_needed > 0 and refineries:
            # Prefer idle workers; then poach from mineral trips.
            pool: list[dict] = list(idle)
            if len(pool) < gas_needed:
                pool.extend([u for u in wu
                             if u["order"] in MINERAL_ORDERS
                             and u not in pool])
            reassigned_ids: set[int] = set()
            for w in pool[:gas_needed]:
                target = nearest(w, refineries)
                if target is None:
                    continue
                try:
                    ack = await c.gather(unit_id=w["unit_id"],
                                         target_unit=target["unit_id"])
                    print(f"[miner]  {unit_type_name(w['type'])} "
                          f"{w['unit_id']} -> gas {target['unit_id']} "
                          f"@frame={ack['queued_at_frame']}")
                    reassigned_ids.add(w["unit_id"])
                except Exception as e:
                    print(f"[miner]  cmd error: {e}")
            # Any workers we just reassigned shouldn't also be sent to
            # minerals this tick.
            idle = [u for u in idle if u["unit_id"] not in reassigned_ids]

        # ---- 2. Send the rest of the idle workers to minerals. ----
        if mfs:
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


async def main(api_key: str, host: str, port: int,
               interval_sec: float, gas_workers: int) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, gas_workers)


def entrypoint() -> None:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.agents.miner")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=2.0)
    p.add_argument("--gas-workers", type=int, default=3,
                   help="target workers per completed refinery")
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port,
                         args.interval_sec, args.gas_workers))
    except KeyboardInterrupt:
        print("\n[miner] stopped")


if __name__ == "__main__":
    entrypoint()
