"""Auto-miner agent: send every idle worker to gather from the nearest
mineral field.

Educational example. Currently uses the "attack" verb targeting a
mineral field, which in retail BW would be interpreted client-side as
"gather". Our sim does NOT do that translation -- the probe treats it
as an "attack this thing" order, moves to the mineral, and idles once
it gets there without actually mining. Getting real mining to work
needs one of:

  1. A dedicated "gather" verb in server/agent_protocol.h that emits
     the specific gather-order bytes (Orders::MoveToMinerals /
     Harvest1). This is the right long-term fix.
  2. Client-side: detect the probe reached the mineral, then re-issue
     a follow-up order. Fragile.

Kept as-is for now because it demonstrates the observe -> command loop
and shows what "silent no-op" looks like when a command isn't quite
right (see the `mining=0` count staying at zero).

Usage:
    python3 -m python_agent.agents.miner <api_key>
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from python_agent.client import Client
from python_agent.enums import (
    WORKER_TYPES, IDLE_ORDERS,
    unit_type_name, order_name, ORDERS_BY_NAME,
)


# Orders that mean "worker is already busy mining or on its way". If a
# worker has one of these, we leave it alone.
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


def _dist_sq(a: dict, b: dict) -> int:
    dx = a["x"] - b["x"]
    dy = a["y"] - b["y"]
    return dx * dx + dy * dy


async def main(api_key: str, host: str, port: int, interval_sec: float) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        print(f"[miner] connected as slot={c.welcome.slot} "
              f"at frame={c.welcome.current_frame}")

        while True:
            # Include enemies so neutrals (mineral fields) come through.
            # The server bundles both under the same code path.
            obs = await c.observe(targets=["units", "resources", "enemies"])
            # Neutrals list has mineral fields + geysers. Filter to just
            # mineral fields by name -- there are 3 variants (Type
            # Mineral_Field, Mineral_Field_Type_2, Mineral_Field_Type_3).
            mineral_field_ids = {
                _id for _id, name in (
                    (176, "Resource_Mineral_Field"),
                    (177, "Resource_Mineral_Field_Type_2"),
                    (178, "Resource_Mineral_Field_Type_3"),
                )
            }
            minerals = [n for n in obs.get("neutrals", [])
                        if n["type"] in mineral_field_ids]
            if not minerals:
                # No visible minerals in the observation. Common for a
                # fresh spawn before any vision -- wait it out.
                print(f"[miner] frame={obs['current_frame']} "
                      "no minerals in view yet")
                await asyncio.sleep(interval_sec)
                continue

            workers = [u for u in obs["units"] if u["type"] in WORKER_TYPES]
            idle = [w for w in workers
                    if w["order"] in IDLE_ORDERS
                    or w["order"] not in MINING_ORDERS]
            # Only re-command truly idle ones -- don't disrupt workers
            # already on a mining trip.
            idle = [w for w in idle if w["order"] in IDLE_ORDERS]
            mining_now = sum(1 for w in workers if w["order"] in MINING_ORDERS)
            print(f"[miner] frame={obs['current_frame']} "
                  f"workers={len(workers)} mining={mining_now} "
                  f"idle={len(idle)} minerals={obs['resources']['minerals']}")

            for w in idle:
                nearest = min(minerals, key=lambda m: _dist_sq(w, m))
                try:
                    ack = await c.attack(
                        unit_id=w["unit_id"],
                        target_unit=nearest["unit_id"],
                    )
                    print(f"[miner]  {unit_type_name(w['type'])} "
                          f"{w['unit_id']} -> mineral {nearest['unit_id']} "
                          f"@frame={ack['queued_at_frame']}")
                except Exception as e:
                    print(f"[miner]  cmd error: {e}")

            await asyncio.sleep(interval_sec)


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
