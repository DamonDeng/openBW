"""Trainer agent: keep the main production building busy by training
workers until we hit supply cap or run out of minerals.

Race-aware: Terran CC trains SCV, Protoss Nexus trains Probe, Zerg
Larva morphs into Drone. It's a dumb loop -- issues a train command
every tick if a producer is idle and there's a chance we can afford
it. The sim silently rejects if we can't; we retry next tick.

Usage:
    python3 -m python_agent.agents.trainer <api_key>
"""

from __future__ import annotations

import argparse
import asyncio

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name, order_name,
    ORDERS_BY_NAME, UNIT_TYPES_BY_NAME,
)
from python_agent.helpers import guess_race


# race -> (producer type ids, unit type to train, mineral cost, supply cost)
RACE_PRODUCERS = {
    "terran": {
        "producers": {UNIT_TYPES_BY_NAME["Terran_Command_Center"]},
        "worker":    UNIT_TYPES_BY_NAME["Terran_SCV"],
        "cost_min":  50,
        "cost_supply": 2,   # supply values are in half-units
    },
    "protoss": {
        "producers": {UNIT_TYPES_BY_NAME["Protoss_Nexus"]},
        "worker":    UNIT_TYPES_BY_NAME["Protoss_Probe"],
        "cost_min":  50,
        "cost_supply": 2,
    },
    "zerg": {
        # Zerg workers come from morphing a Larva, which is a unit not a
        # building. Larva sits inside a Hatchery and shows up as an own
        # unit with type Zerg_Larva when idle.
        "producers": {UNIT_TYPES_BY_NAME["Zerg_Larva"]},
        "worker":    UNIT_TYPES_BY_NAME["Zerg_Drone"],
        "cost_min":  50,
        "cost_supply": 2,
    },
}


# Orders a completed producer sits in when idle.
IDLE_ORDERS: set[int] = {
    ORDERS_BY_NAME["Guard"],
    ORDERS_BY_NAME["PlayerGuard"],
    ORDERS_BY_NAME["Nothing"],
}


async def run(c: Client, interval_sec: float) -> None:
    print(f"[trainer] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    race = None
    plan = None

    while True:
        obs = await c.observe(targets=["units", "resources"])

        if race is None:
            race = guess_race(obs["units"])
            plan = RACE_PRODUCERS.get(race)
            print(f"[trainer] inferred race={race} plan={plan!r}")
            if plan is None:
                print(f"[trainer] don't know how to train for race={race}; exiting")
                return

        # Any idle producer we can push a train order to?
        idle_producers = [
            u for u in obs["units"]
            if u["type"] in plan["producers"]
            and u["order"] in IDLE_ORDERS
            and u.get("completed", True)
        ]

        # We can't perfectly predict cost -- sim may reject silently if
        # we're supply-capped or short. Just try each idle producer.
        r = obs["resources"]
        can_afford = (r["minerals"] >= plan["cost_min"]
                      and r["supply_used"] + plan["cost_supply"] <= r["supply_max"])
        print(f"[trainer] frame={obs['current_frame']} "
              f"race={race} min={r['minerals']} supply={r['supply_used']}/{r['supply_max']} "
              f"idle_producers={len(idle_producers)} can_afford={can_afford}")

        if can_afford and idle_producers:
            for p in idle_producers[:1]:  # one per tick, avoid double-pop on same producer
                try:
                    ack = await c.train(unit_id=p["unit_id"],
                                        unit_type=plan["worker"])
                    print(f"[trainer]  {unit_type_name(p['type'])} "
                          f"{p['unit_id']} -> train {unit_type_name(plan['worker'])} "
                          f"@frame={ack['queued_at_frame']}")
                except Exception as e:
                    print(f"[trainer]  cmd error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int, interval_sec: float) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec)


def entrypoint() -> None:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.agents.trainer")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=2.0)
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port, args.interval_sec))
    except KeyboardInterrupt:
        print("\n[trainer] stopped")


if __name__ == "__main__":
    entrypoint()
