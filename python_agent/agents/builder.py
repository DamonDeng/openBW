"""Builder agent: places a supply-cap building near the main structure
using the server's find_placement query to pick a valid tile.

Race-aware:
  Terran   -> Supply Depot
  Protoss  -> Pylon
  Zerg     -> Overlord (train from Larva; no build placement needed)

Loop:
  1. observe -> know minerals, workers, main structure, existing supply
     buildings.
  2. If we already have enough supply capacity, wait.
  3. If we can afford the next supply building, query find_placement
     for a valid tile near our main structure.
  4. Pick the first (nearest) tile from the result. Issue the build.
  5. Remember the worker we chose so we don't interrupt it on the next
     tick with a new build command.

Runs alongside miner + trainer to keep the economy going.

Usage:
    python3 -m python_agent.agents.builder <api_key>
"""

from __future__ import annotations

import argparse
import asyncio

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name, ORDERS_BY_NAME, UNIT_TYPES_BY_NAME,
)
from python_agent.helpers import (
    guess_race, workers, buildings,
)


# Orders a worker enters after being told to build. If our chosen
# worker is in any of these, our previous build order is still in
# progress -- don't spam another.
BUILDING_ORDERS: set[int] = {
    ORDERS_BY_NAME["ConstructingBuilding"],
    ORDERS_BY_NAME["PlaceBuilding"],
    ORDERS_BY_NAME["PlaceProtossBuilding"],
    ORDERS_BY_NAME["DroneStartBuild"],
    ORDERS_BY_NAME["DroneBuild"],
    ORDERS_BY_NAME["Move"],   # walking to the placement tile
}


# Race -> supply-building plan.
#   'building': unit_type id of what we build
#   'cost_min': minerals cost per structure
#   'mode':     'build' for Terran/Protoss (place via worker),
#               'train' for Zerg (morph Larva into Overlord)
#   'main':    set of unit_type ids used to find our base
#              anchor for placement search.
RACE_PLANS = {
    "terran": {
        "building": UNIT_TYPES_BY_NAME["Terran_Supply_Depot"],
        "cost_min": 100,
        "mode":     "build",
        "main":     {UNIT_TYPES_BY_NAME["Terran_Command_Center"]},
    },
    "protoss": {
        "building": UNIT_TYPES_BY_NAME["Protoss_Pylon"],
        "cost_min": 100,
        "mode":     "build",
        "main":     {UNIT_TYPES_BY_NAME["Protoss_Nexus"]},
    },
    "zerg": {
        "building": UNIT_TYPES_BY_NAME["Zerg_Overlord"],
        "cost_min": 100,
        "mode":     "train",
        "main":     {UNIT_TYPES_BY_NAME["Zerg_Larva"]},
    },
}


def _building_or_pending(u: dict) -> bool:
    """True if this unit is either flagged as a building on the wire
    or has an order that means 'a building of me is being placed'."""
    if u.get("building"):
        return True
    return u["order"] in BUILDING_ORDERS


async def run(c: Client, interval_sec: float, target_supply_gap: int) -> None:
    print(f"[builder] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    race = None
    plan = None
    outstanding_worker: int | None = None  # worker with a build order in flight

    while True:
        obs = await c.observe(targets=["units", "resources"])

        if race is None:
            race = guess_race(obs["units"])
            plan = RACE_PLANS.get(race)
            print(f"[builder] inferred race={race}")
            if plan is None:
                print("[builder] no plan for this race; exiting")
                return

        r = obs["resources"]

        # Count owned supply buildings. A unit is in-progress when the
        # server explicitly omits the "completed" flag; when the flag is
        # set (default true), it's built. This keeps the two counts
        # disjoint.
        target_type = plan["building"]
        matching = [u for u in obs["units"] if u["type"] == target_type]
        have_completed = sum(1 for u in matching if u.get("completed") is True)
        have_in_progress = sum(1 for u in matching if not u.get("completed", False))

        print(f"[builder] frame={obs['current_frame']} race={race} "
              f"min={r['minerals']} supply={r['supply_used']}/{r['supply_max']} "
              f"have {unit_type_name(target_type)}={have_completed} "
              f"(+{have_in_progress} in progress)")

        # Do we need more supply?
        # - Always try to keep >=3 free (build proactively).
        # - AND cap the total build attempts so we don't cover the map
        #   with pylons -- a max of ~6 supply buildings is plenty for
        #   feature-coverage purposes.
        gap = r["supply_max"] - r["supply_used"]
        need_more = (gap < target_supply_gap
                     and have_in_progress == 0
                     and (have_completed + have_in_progress) < 6)

        if not need_more:
            await asyncio.sleep(interval_sec)
            continue

        # Need to hold back some minerals so the trainer can keep making
        # workers. If we spend everything on a Pylon at 100 min we starve
        # the trainer at 50 for a probe. Wait until we have cost_min + 60.
        if r["minerals"] < plan["cost_min"] + 60:
            await asyncio.sleep(interval_sec)
            continue

        # If we've already dispatched a worker and it's still walking
        # or building, don't re-dispatch.
        if outstanding_worker is not None:
            still_busy = any(
                u["unit_id"] == outstanding_worker and u["order"] in BUILDING_ORDERS
                for u in obs["units"]
            )
            if still_busy:
                await asyncio.sleep(interval_sec)
                continue
            outstanding_worker = None

        if plan["mode"] == "train":
            # Zerg overlord path: find a Larva and morph.
            larvas = [u for u in obs["units"] if u["type"] in plan["main"]]
            if not larvas:
                await asyncio.sleep(interval_sec)
                continue
            try:
                ack = await c.train(unit_id=larvas[0]["unit_id"],
                                    unit_type=target_type)
                print(f"[builder]  morph Larva {larvas[0]['unit_id']} "
                      f"-> {unit_type_name(target_type)} "
                      f"@frame={ack['queued_at_frame']}")
            except Exception as e:
                print(f"[builder]  cmd error: {e}")
            await asyncio.sleep(interval_sec)
            continue

        # Terran / Protoss build path.
        wu = workers(obs["units"])
        if not wu:
            await asyncio.sleep(interval_sec)
            continue
        worker = wu[0]

        # Ask the server where we can put one.
        try:
            resp = await c.find_placement(
                unit_type=target_type,
                worker_unit=worker["unit_id"],
                radius_tiles=15,
                max_results=8,
            )
        except Exception as e:
            print(f"[builder]  find_placement error: {e}")
            await asyncio.sleep(interval_sec)
            continue

        spots = resp.get("spots", [])
        if not spots:
            print(f"[builder]  no valid placement found within 15 tiles; "
                  f"widening search would go here")
            await asyncio.sleep(interval_sec)
            continue

        # Take the nearest one.
        spot = spots[0]
        try:
            ack = await c.build(unit_id=worker["unit_id"],
                                unit_type=target_type,
                                tile_x=spot["tile_x"],
                                tile_y=spot["tile_y"])
            outstanding_worker = worker["unit_id"]
            print(f"[builder]  worker {worker['unit_id']} -> place "
                  f"{unit_type_name(target_type)} @tile "
                  f"({spot['tile_x']},{spot['tile_y']}) "
                  f"@frame={ack['queued_at_frame']}")
        except Exception as e:
            print(f"[builder]  cmd error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int,
               interval_sec: float, gap: int) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, gap)


def entrypoint() -> None:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.agents.builder")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=2.0)
    p.add_argument("--supply-gap", type=int, default=3,
                   help="build another supply building when "
                        "(supply_max - supply_used) drops below this")
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port,
                         args.interval_sec, args.supply_gap))
    except KeyboardInterrupt:
        print("\n[builder] stopped")


if __name__ == "__main__":
    entrypoint()
