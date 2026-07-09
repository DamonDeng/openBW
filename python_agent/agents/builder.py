"""Builder agent: picks a worker and orders it to place a supply-cap
building near its main structure.

Terran -> Supply Depot,  Protoss -> Pylon,  Zerg -> Overlord (special:
Zerg supply comes from morphing a Larva, not a build placement, so
zerg falls back to training an Overlord like the trainer agent does).

DEMO / LEARNING SCAFFOLD, not a working economy bot.

BW's `build` command is finicky: the placement tile must be buildable,
unoccupied, and (for Protoss non-Pylons) in range of a Pylon. This
agent picks a fixed offset from the main structure and repeatedly
tries; you'll see the build command going over the wire in the log,
but for Protoss on Bottleneck the fixed offset often lands on
minerals or unbuildable ground and the sim silently rejects it. The
verb, wire path, and worker selection all work -- the placement math
is the workshop attendee's problem to solve.

Realistic follow-ups:
  1. Scan the map for a buildable tile near the main structure
     (check tile flags for buildable + unoccupied).
  2. Watch for the worker's order to transition to ConstructingBuilding
     as the signal of success; retry with a different tile otherwise.
  3. Track worker occupancy explicitly so we don't perpetually
     interrupt one worker with new build commands.

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
    guess_race, workers, pixel_to_tile,
)


# Orders a worker enters after being told to build.
BUILDING_ORDERS: set[int] = {
    ORDERS_BY_NAME["ConstructingBuilding"],
    ORDERS_BY_NAME["PlaceBuilding"],
    ORDERS_BY_NAME["PlaceProtossBuilding"],
    ORDERS_BY_NAME["DroneStartBuild"],
    ORDERS_BY_NAME["DroneBuild"],
}


# Race -> (main structure ids to anchor placement, building unit id,
# mineral cost, tile offset from main to place at).
RACE_SUPPLY = {
    "terran": {
        "main":     {UNIT_TYPES_BY_NAME["Terran_Command_Center"]},
        "building": UNIT_TYPES_BY_NAME["Terran_Supply_Depot"],
        "cost_min": 100,
        "offset":   (0, 3),   # 3 tiles down
    },
    "protoss": {
        "main":     {UNIT_TYPES_BY_NAME["Protoss_Nexus"]},
        "building": UNIT_TYPES_BY_NAME["Protoss_Pylon"],
        "cost_min": 100,
        # 5 tiles down. Pylons need to be far enough from the Nexus
        # to not overlap; not so far that the worker has to trek across
        # the map to reach the site.
        "offset":   (0, 5),
    },
    # Zerg: no build verb needed for supply; morph a Larva into an
    # Overlord via train. Handled below.
    "zerg": {
        "main":     {UNIT_TYPES_BY_NAME["Zerg_Larva"]},
        "building": UNIT_TYPES_BY_NAME["Zerg_Overlord"],
        "cost_min": 100,
        "offset":   None,     # signal: use train not build
    },
}


async def run(c: Client, interval_sec: float) -> None:
    print(f"[builder] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    race = None
    plan = None
    already_ordered = False   # simple: place one depot then keep watching.

    while True:
        obs = await c.observe(targets=["units", "resources"])

        if race is None:
            race = guess_race(obs["units"])
            plan = RACE_SUPPLY.get(race)
            print(f"[builder] inferred race={race}")
            if plan is None:
                print("[builder] no plan for this race; exiting")
                return

        r = obs["resources"]

        # Recount how many supply structures we own. Rough proxy: number
        # of completed buildings of the right type in own units.
        target_building = plan["building"]
        have = sum(
            1 for u in obs["units"]
            if u["type"] == target_building
        )
        # Ceiling: don't spam the whole map with pylons.
        print(f"[builder] frame={obs['current_frame']} "
              f"race={race} min={r['minerals']} "
              f"supply={r['supply_used']}/{r['supply_max']} "
              f"have_{unit_type_name(target_building)}={have}")

        if have >= 4:
            # We've built plenty; go quiet.
            await asyncio.sleep(interval_sec)
            continue

        if r["minerals"] < plan["cost_min"]:
            await asyncio.sleep(interval_sec)
            continue

        if plan["offset"] is None:
            # Zerg overlord: train from an idle Larva.
            larvas = [u for u in obs["units"] if u["type"] in plan["main"]]
            if larvas:
                try:
                    ack = await c.train(unit_id=larvas[0]["unit_id"],
                                        unit_type=plan["building"])
                    print(f"[builder]  morph Larva {larvas[0]['unit_id']} -> "
                          f"{unit_type_name(plan['building'])} "
                          f"@frame={ack['queued_at_frame']}")
                except Exception as e:
                    print(f"[builder]  cmd error: {e}")
        else:
            # Terran/Protoss: find a worker + a main structure + a spot.
            mains = [u for u in obs["units"] if u["type"] in plan["main"]]
            wu = workers(obs["units"])
            if not mains or not wu:
                await asyncio.sleep(interval_sec)
                continue

            # If any worker is already walking-to-build or actively
            # building, don't issue another command -- give the previous
            # order time to complete. Otherwise we perpetually interrupt.
            walking_to_build = any(
                w["order"] in BUILDING_ORDERS or
                # Move + we recently told them to build; approximate by
                # skipping if any worker isn't in a normal gather/idle
                # state. Better: track our own outstanding orders.
                w["order"] == ORDERS_BY_NAME["Move"]
                for w in wu
            )
            if walking_to_build:
                await asyncio.sleep(interval_sec)
                continue

            main = mains[0]
            worker = wu[0]
            main_tx, main_ty = pixel_to_tile(main["x"], main["y"])
            dx, dy = plan["offset"]
            # Shift a bit for each extra depot so they don't stack.
            tile_x = main_tx + dx + (have * 2)
            tile_y = main_ty + dy
            try:
                ack = await c.build(unit_id=worker["unit_id"],
                                    unit_type=plan["building"],
                                    tile_x=tile_x, tile_y=tile_y)
                print(f"[builder]  worker {worker['unit_id']} -> "
                      f"place {unit_type_name(plan['building'])} @tile "
                      f"({tile_x},{tile_y}) @frame={ack['queued_at_frame']}")
            except Exception as e:
                print(f"[builder]  cmd error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int, interval_sec: float) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec)


def entrypoint() -> None:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.agents.builder")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=3.0)
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port, args.interval_sec))
    except KeyboardInterrupt:
        print("\n[builder] stopped")


if __name__ == "__main__":
    entrypoint()
