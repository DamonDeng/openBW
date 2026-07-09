"""Builder agent: walks a race-specific build order.

Each race has a fixed opening sequence:

  Terran:   Supply_Depot -> Refinery -> Barracks
  Protoss:  Pylon        -> Assimilator -> Gateway -> Cybernetics_Core
  Zerg:     Overlord     -> Extractor -> Spawning_Pool

The builder walks the list top-to-bottom. Each step:
  - "supply":  worker places a supply building; extra copies as supply
                fills up.
  - "gas":     worker places a refinery ON a vespene geyser.
  - "producer" / "tech": worker places once (max_count=1); enables
                producing combat units + tech unlocks.

Overlord for Zerg is a Larva morph via `train` (no build verb path).

Every step queries the server's find_placement for a valid tile, so
we never guess coordinates. That handles power/creep/geyser rules
automatically.

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
    guess_race, workers, vespene_geysers,
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
    ORDERS_BY_NAME["Move"],  # walking to the placement tile
}


# Per-race build queue. Each entry:
#   kind:      "supply"   -- extra copies as supply cap fills
#              "gas"      -- placed on a vespene geyser
#              "producer" -- e.g. Barracks/Gateway/Spawning_Pool
#              "tech"     -- e.g. Cybernetics_Core
#   type_id:   the unit_type of what we build
#   cost_min:  mineral cost
#   mode:      "build"    -- worker places via find_placement
#              "morph"    -- Zerg drone morphs INTO the building
#              "train"    -- Zerg larva morphs into a unit (Overlord)
#   anchor:    unit_type_ids near which to search for a placement.
#              For gas, we anchor on a geyser instead.
#   max_count: 1 for producer/tech; None for unlimited (supply).
RACE_BUILD_ORDER = {
    "protoss": [
        {"kind": "supply",   "type_id": UNIT_TYPES_BY_NAME["Protoss_Pylon"],
         "cost_min": 100,     "mode": "build",
         "anchor":  {UNIT_TYPES_BY_NAME["Protoss_Nexus"]},
         "max_count": None},
        {"kind": "gas",      "type_id": UNIT_TYPES_BY_NAME["Protoss_Assimilator"],
         "cost_min": 100,     "mode": "build",
         "anchor":  None,     "max_count": 1},
        {"kind": "producer", "type_id": UNIT_TYPES_BY_NAME["Protoss_Gateway"],
         "cost_min": 150,     "mode": "build",
         "anchor":  {UNIT_TYPES_BY_NAME["Protoss_Pylon"]},
         "max_count": 1},
        {"kind": "tech",     "type_id": UNIT_TYPES_BY_NAME["Protoss_Cybernetics_Core"],
         "cost_min": 200,     "mode": "build",
         "anchor":  {UNIT_TYPES_BY_NAME["Protoss_Pylon"]},
         "max_count": 1},
    ],
    "terran": [
        {"kind": "supply",   "type_id": UNIT_TYPES_BY_NAME["Terran_Supply_Depot"],
         "cost_min": 100,     "mode": "build",
         "anchor":  {UNIT_TYPES_BY_NAME["Terran_Command_Center"]},
         "max_count": None},
        {"kind": "gas",      "type_id": UNIT_TYPES_BY_NAME["Terran_Refinery"],
         "cost_min": 100,     "mode": "build",
         "anchor":  None,     "max_count": 1},
        {"kind": "producer", "type_id": UNIT_TYPES_BY_NAME["Terran_Barracks"],
         "cost_min": 150,     "mode": "build",
         "anchor":  {UNIT_TYPES_BY_NAME["Terran_Command_Center"]},
         "max_count": 1},
    ],
    "zerg": [
        {"kind": "supply",   "type_id": UNIT_TYPES_BY_NAME["Zerg_Overlord"],
         "cost_min": 100,     "mode": "train",
         "anchor":  {UNIT_TYPES_BY_NAME["Zerg_Larva"]},
         "max_count": None},
        {"kind": "gas",      "type_id": UNIT_TYPES_BY_NAME["Zerg_Extractor"],
         "cost_min": 50,      "mode": "morph",
         "anchor":  None,     "max_count": 1},
        {"kind": "producer", "type_id": UNIT_TYPES_BY_NAME["Zerg_Spawning_Pool"],
         "cost_min": 200,     "mode": "morph",
         "anchor":  {UNIT_TYPES_BY_NAME["Zerg_Hatchery"]},
         "max_count": 1},
    ],
}


def _count_of(units: list[dict], type_id: int) -> tuple[int, int]:
    """Return (completed, in_progress) counts for a given unit_type."""
    matching = [u for u in units if u["type"] == type_id]
    completed = sum(1 for u in matching if u.get("completed") is True)
    in_progress = sum(1 for u in matching if not u.get("completed", False))
    return completed, in_progress


def _next_step(race_plan: list[dict], units: list[dict], resources: dict,
               supply_gap_trigger: int) -> dict | None:
    """Decide which step to work on next. Returns the step dict or None."""
    for step in race_plan:
        completed, in_progress = _count_of(units, step["type_id"])
        max_count = step["max_count"]

        if max_count is not None and (completed + in_progress) >= max_count:
            # Cap reached; move on.
            continue

        if step["kind"] == "supply":
            # Build another one when supply gap gets low. Skip if we
            # already have one in flight.
            gap = resources["supply_max"] - resources["supply_used"]
            if gap >= supply_gap_trigger:
                # Enough headroom; move to next step in the queue.
                continue
            if in_progress > 0:
                # A supply building is already coming; wait.
                continue
            return step
        else:
            # Non-supply steps are 1-shot: build if we haven't started yet.
            if completed > 0 or in_progress > 0:
                continue
            return step
    return None


async def _place_via_worker(c: Client, step: dict, obs: dict) -> int | None:
    """Ask find_placement + issue build. Returns the worker unit_id we
    picked, or None on any failure. Caller records this to avoid
    re-issuing while the previous order is in flight."""
    wu = workers(obs["units"])
    if not wu:
        return None
    worker = wu[0]

    kwargs: dict = {
        "unit_type":     step["type_id"],
        "worker_unit":   worker["unit_id"],
        "radius_tiles":  20,
        "max_results":   8,
    }
    # Gas: search around a geyser, not near an owned building.
    if step["kind"] == "gas":
        geysers = vespene_geysers(obs.get("neutrals", []))
        if not geysers:
            return None
        g = geysers[0]
        kwargs["worker_unit"] = worker["unit_id"]
        kwargs["center_x"] = g["x"]
        kwargs["center_y"] = g["y"]
        kwargs["radius_tiles"] = 3

    try:
        resp = await c.find_placement(**kwargs)
    except Exception as e:
        print(f"[builder]  find_placement error: {e}")
        return None

    spots = resp.get("spots", [])
    if not spots:
        print(f"[builder]  no valid placement for {unit_type_name(step['type_id'])}")
        return None

    spot = spots[0]
    try:
        ack = await c.build(unit_id=worker["unit_id"],
                            unit_type=step["type_id"],
                            tile_x=spot["tile_x"],
                            tile_y=spot["tile_y"])
        print(f"[builder]  worker {worker['unit_id']} -> place "
              f"{unit_type_name(step['type_id'])} @tile "
              f"({spot['tile_x']},{spot['tile_y']}) "
              f"@frame={ack['queued_at_frame']}")
        return worker["unit_id"]
    except Exception as e:
        print(f"[builder]  cmd error: {e}")
        return None


async def _morph_zerg(c: Client, step: dict, obs: dict) -> int | None:
    """Zerg drone morphs INTO a building. Uses `build` for extractor
    (targeting the geyser) or a drone-on-tile placement for structures.
    Also handles the Overlord case via `train` on a Larva."""
    if step["mode"] == "train":
        # Overlord morph. Larva is the anchor.
        larvas = [u for u in obs["units"] if u["type"] in step["anchor"]]
        if not larvas:
            return None
        try:
            ack = await c.train(unit_id=larvas[0]["unit_id"],
                                unit_type=step["type_id"])
            print(f"[builder]  Larva {larvas[0]['unit_id']} -> morph "
                  f"{unit_type_name(step['type_id'])} @frame={ack['queued_at_frame']}")
            return larvas[0]["unit_id"]
        except Exception as e:
            print(f"[builder]  cmd error: {e}")
            return None
    else:
        # Drone morph. Placement search anchored on hatchery (or geyser).
        return await _place_via_worker(c, step, obs)


async def run(c: Client, interval_sec: float, supply_gap_trigger: int) -> None:
    print(f"[builder] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    race = None
    plan: list[dict] | None = None
    outstanding_worker: int | None = None

    while True:
        obs = await c.observe(targets=["units", "resources", "enemies"])

        if race is None:
            race = guess_race(obs["units"])
            plan = RACE_BUILD_ORDER.get(race)
            print(f"[builder] inferred race={race}")
            if plan is None:
                print("[builder] no plan for this race; exiting")
                return

        r = obs["resources"]

        # Summary line for observability.
        summary = ", ".join(
            f"{unit_type_name(s['type_id'])[:24]}={ _count_of(obs['units'], s['type_id'])[0]}"
            f"(+{ _count_of(obs['units'], s['type_id'])[1]})"
            for s in plan
        )
        print(f"[builder] frame={obs['current_frame']} race={race} "
              f"min={r['minerals']} gas={r['gas']} "
              f"supply={r['supply_used']}/{r['supply_max']}  {summary}")

        # Still waiting on our previous worker?
        if outstanding_worker is not None:
            still_busy = any(
                u["unit_id"] == outstanding_worker and u["order"] in BUILDING_ORDERS
                for u in obs["units"]
            )
            if still_busy:
                await asyncio.sleep(interval_sec)
                continue
            outstanding_worker = None

        step = _next_step(plan, obs["units"], r, supply_gap_trigger)
        if step is None:
            # All steps satisfied; wait for something to change.
            await asyncio.sleep(interval_sec)
            continue

        # Only try if we can afford it, plus a bit of headroom so the
        # trainer doesn't starve.
        headroom = 40 if step["kind"] == "producer" else 20
        if r["minerals"] < step["cost_min"] + headroom:
            await asyncio.sleep(interval_sec)
            continue

        if race == "zerg":
            outstanding_worker = await _morph_zerg(c, step, obs)
        else:
            outstanding_worker = await _place_via_worker(c, step, obs)

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int,
               interval_sec: float, supply_gap: int) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, supply_gap)


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
