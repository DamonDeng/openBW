"""p_agent_v2: exploratory, coverage-oriented agent.

The point of this agent is NOT to play well. It's to hit every
train/build verb combination for a given race so we can:

  1. Confirm the API round-trip for each unit/building type.
  2. Discover which commands the sim silently drops (and why) by
     comparing our command stream against the observation deltas.
  3. Cover placement, resource, prereq, and supply edge cases without
     hand-writing a build order.

Strategy: priority-ordered goals with mineral/gas reservation. Each
tick we walk a list of Goals top-to-bottom. If a goal is unsatisfied
and its cost fits in the remaining tick budget, it fires ONE command
and reserves its cost so lower-priority goals see reduced budget. That
prevents "spent 200 on Cyber Core so no minerals left for Pylon" bugs
when income briefly touches an expensive threshold.

Priority order (highest first):
   1. mining      -- idle workers -> nearest mineral (free)
   2. pylons/supply -- keep 20 pylons OR (supply_used + slack) worth
   3. workers     -- 40 probes total
   4. gas structure -- one Assimilator/Refinery
   5. gas workers -- 3 per completed refinery
   6. catalog buildings -- one of each in tech-tree order
   7. catalog units  -- one of each producer/unit pair
   8. attack      -- idle combat units -> enemy corner
   9. coverage verbs -- one move + one stop for API smoke coverage

Each "try" fires a command optimistically. We don't check prereqs
client-side -- if the sim silently rejects, we detect that by
comparing the next observation's count / resource-delta and log the
reject. Rate-limited: every 5th failure per (verb, target).

Usage:
    python3 -m python_agent.agents.p_agent_v2 <api_key>
"""

from __future__ import annotations

import argparse
import asyncio
import random
from collections import defaultdict
from dataclasses import dataclass, field

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name, ORDERS_BY_NAME, UNIT_TYPES_BY_NAME, IDLE_ORDERS,
)
from python_agent.helpers import (
    guess_race, workers, mineral_fields, vespene_geysers, buildings,
    nearest, combat_units, own_refineries,
)


# --------------------------------------------------------------------
# Race catalogs -- everything we want to try to make.
# --------------------------------------------------------------------

@dataclass
class BuildingSpec:
    type_id: int
    cost_min: int
    cost_gas: int = 0
    anchor: str = "any"          # "nexus" | "pylon" | "geyser" | "any"

@dataclass
class UnitSpec:
    type_id: int
    producer_type_id: int
    cost_min: int
    cost_gas: int = 0
    supply_each: int = 0         # in BW-nominal (integer wire value)


# Note: many of these have prereqs (Cybernetics Core needs Gateway;
# Templar Archives needs Citadel; ...) but we don't encode them. We
# fire the build attempt and log the reject if the sim drops it.
PROTOSS_BUILDINGS: list[BuildingSpec] = [
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Gateway"],            150, 0,   "nexus"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Forge"],              150, 0,   "nexus"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Cybernetics_Core"],   200, 0,   "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Photon_Cannon"],      150, 0,   "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Shield_Battery"],     100, 0,   "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Citadel_of_Adun"],    150, 100, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"],  200, 200, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Stargate"],           150, 150, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Templar_Archives"],   150, 200, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Observatory"],         50, 100, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Robotics_Support_Bay"],150, 100, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Fleet_Beacon"],       300, 200, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Arbiter_Tribunal"],   200, 150, "pylon"),
]

PROTOSS_UNITS: list[UnitSpec] = [
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Zealot"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"], 100, 0, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Dragoon"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"], 125, 50, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_High_Templar"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"], 50, 150, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Dark_Templar"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"], 125, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Reaver"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"], 200, 100, 4),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Observer"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"], 25, 75, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Shuttle"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"], 200, 0, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Scout"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"], 275, 125, 3),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Corsair"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"], 150, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Carrier"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"], 350, 250, 6),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Arbiter"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"], 100, 350, 4),
]

TERRAN_BUILDINGS: list[BuildingSpec] = [
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Barracks"],           150, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Engineering_Bay"],    125, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Missile_Turret"],      75, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Bunker"],             100, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Academy"],            150, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Factory"],            200, 100, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Starport"],           150, 100, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Armory"],             100, 50,  "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Science_Facility"],   100, 150, "cc"),
]

TERRAN_UNITS: list[UnitSpec] = [
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Marine"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"], 50, 0, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Firebat"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"], 50, 25, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Medic"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"], 50, 25, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Ghost"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"], 25, 75, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Vulture"],
             UNIT_TYPES_BY_NAME["Terran_Factory"], 75, 0, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"],
             UNIT_TYPES_BY_NAME["Terran_Factory"], 150, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Goliath"],
             UNIT_TYPES_BY_NAME["Terran_Factory"], 100, 50, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Wraith"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 150, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Dropship"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 100, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Science_Vessel"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 100, 225, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Battlecruiser"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 400, 300, 6),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Valkyrie"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 250, 125, 3),
]


def race_catalogs(race: str) -> tuple[list[BuildingSpec], list[UnitSpec],
                                      int, int, int]:
    """Return (buildings, units, worker_type, supply_type, main_type)."""
    if race == "protoss":
        return (PROTOSS_BUILDINGS, PROTOSS_UNITS,
                UNIT_TYPES_BY_NAME["Protoss_Probe"],
                UNIT_TYPES_BY_NAME["Protoss_Pylon"],
                UNIT_TYPES_BY_NAME["Protoss_Nexus"])
    if race == "terran":
        return (TERRAN_BUILDINGS, TERRAN_UNITS,
                UNIT_TYPES_BY_NAME["Terran_SCV"],
                UNIT_TYPES_BY_NAME["Terran_Supply_Depot"],
                UNIT_TYPES_BY_NAME["Terran_Command_Center"])
    # Zerg is out of scope for v2 (drone-morph placement isn't wired).
    raise SystemExit(f"[p_v2] race={race} not supported by this agent yet")


# --------------------------------------------------------------------
# Pending command tracking -- ground-truth verification of "did this
# actually take effect?"
# --------------------------------------------------------------------

@dataclass
class Pending:
    verb: str                        # "build" | "train"
    target_type: int
    issued_frame: int
    pre_min: int                     # minerals BEFORE fire (from our observation)
    pre_gas: int
    pre_count: int                   # completed + in_progress of target_type
    cost_min: int
    cost_gas: int
    worker_id: int | None = None     # build only

    def label(self) -> str:
        return f"{self.verb}:{unit_type_name(self.target_type)}"


@dataclass
class Stats:
    tried:  dict[str, int] = field(default_factory=lambda: defaultdict(int))
    ack:    dict[str, int] = field(default_factory=lambda: defaultdict(int))
    took:   dict[str, int] = field(default_factory=lambda: defaultdict(int))
    reject: dict[str, int] = field(default_factory=lambda: defaultdict(int))


# --------------------------------------------------------------------
# Helpers.
# --------------------------------------------------------------------

def count_units(units: list[dict], type_id: int) -> tuple[int, int]:
    """(completed, in_progress)."""
    m = [u for u in units if u["type"] == type_id]
    return (sum(1 for u in m if u.get("completed") is True),
            sum(1 for u in m if not u.get("completed", False)))


def find_unit(units: list[dict], unit_id: int) -> dict | None:
    for u in units:
        if u["unit_id"] == unit_id:
            return u
    return None


def own_of_type(units: list[dict], type_id: int, only_complete: bool = True) -> list[dict]:
    return [u for u in units
            if u["type"] == type_id
            and (not only_complete or u.get("completed") is True)]


# --------------------------------------------------------------------
# Verification.
# --------------------------------------------------------------------

def verify_pending(pending: dict, obs: dict, stats: Stats,
                   reject_counts: dict[str, int],
                   grace_frames: int) -> None:
    """Check outstanding commands against the observation and log rejects.

    Called every tick BEFORE we fire new commands. Any pending we can
    confirm (or definitively reject) is popped from the dict.

    We ONLY trust the count-delta signal:
      - build:  in-progress-or-completed of target went up  -> TOOK
      - train:  count of unit type went up                  -> TOOK

    An earlier version also used "minerals debited by ~cost" as a
    faster train signal, but that turned out to be racy: mining
    workers keep depositing minerals in the same tick, so a train
    that DID take might show a smaller debit than expected while a
    train that DIDN'T take can still show mineral drop from other
    causes. Count-based is slower but ground-truth.

    Since count-based is slower, grace_frames MUST be large enough to
    cover:
      - buildings: worker walk-to-tile + first-frame placement
      - trains:    unit spawn time (300+ frames for Probe)
    Caller computes grace from measured tick interval so we never
    reject after only one verify pass -- that would clear pending
    prematurely and cause the worker to be re-yanked before its
    build/train registered in the observation.
    """
    r = obs["resources"]
    frame = obs["current_frame"]
    units = obs["units"]

    to_drop: list[str] = []

    for key, p in pending.items():
        completed, in_progress = count_units(units, p.target_type)
        cur_count = completed + in_progress
        age = frame - p.issued_frame

        if cur_count > p.pre_count:
            stats.took[p.label()] += 1
            print(f"[p_v2] TOOK  {p.label():48s} "
                  f"(count {p.pre_count}->{cur_count} after {age}f)")
            to_drop.append(key)
            continue

        if age >= grace_frames:
            stats.reject[p.label()] += 1
            n = stats.reject[p.label()]
            # Log first occurrence + every 5th.
            if n == 1 or n % 5 == 0:
                print(f"[p_v2] REJECT {p.label():48s} "
                      f"after {age}f (grace={grace_frames}). n={n}. "
                      f"pre: min={p.pre_min} gas={p.pre_gas} "
                      f"count={p.pre_count}. "
                      f"now: min={r['minerals']} gas={r['gas']} "
                      f"supply={r['supply_used']}/{r['supply_max']} "
                      f"count={cur_count}. "
                      f"likely: {_likely_reason(p, r, units)}")
            to_drop.append(key)

    for k in to_drop:
        pending.pop(k, None)


def _likely_reason(p: Pending, r: dict, units: list[dict]) -> str:
    """Heuristic explanation for a silent reject. Not authoritative --
    the sim doesn't tell us, so this is our best guess from state."""
    reasons: list[str] = []
    if r["minerals"] < p.cost_min:
        reasons.append(f"min<{p.cost_min}")
    if r["gas"] < p.cost_gas:
        reasons.append(f"gas<{p.cost_gas}")
    if p.verb == "train":
        # Zerglings come in pairs, supply doubled. Otherwise 2 half-units
        # per nominal supply unit.
        # For coarse estimation, assume supply_each was already accounted
        # for by the specs. We can't recover it from Pending, so skip.
        # (Producer alive?)
        pass
    # Prereq? Photon_Cannon needs Pylon vision; Cyber Core needs Gateway;
    # etc. We list the most common cases.
    tname = unit_type_name(p.target_type)
    prereqs = {
        "Protoss_Cybernetics_Core":   ["Protoss_Gateway"],
        "Protoss_Citadel_of_Adun":    ["Protoss_Cybernetics_Core"],
        "Protoss_Templar_Archives":   ["Protoss_Citadel_of_Adun"],
        "Protoss_Robotics_Support_Bay":["Protoss_Robotics_Facility"],
        "Protoss_Observatory":        ["Protoss_Robotics_Facility"],
        "Protoss_Fleet_Beacon":       ["Protoss_Stargate"],
        "Protoss_Arbiter_Tribunal":   ["Protoss_Stargate",
                                       "Protoss_Templar_Archives"],
        "Protoss_Stargate":           ["Protoss_Cybernetics_Core"],
        "Protoss_Robotics_Facility":  ["Protoss_Cybernetics_Core"],
        "Protoss_Photon_Cannon":      ["Protoss_Forge"],
        "Protoss_Shield_Battery":     ["Protoss_Gateway"],
        "Protoss_Dragoon":            ["Protoss_Cybernetics_Core"],
        "Protoss_High_Templar":       ["Protoss_Templar_Archives"],
        "Protoss_Dark_Templar":       ["Protoss_Templar_Archives"],
        "Protoss_Reaver":             ["Protoss_Robotics_Support_Bay"],
        "Protoss_Observer":           ["Protoss_Observatory"],
        "Protoss_Corsair":            ["Protoss_Stargate"],
        "Protoss_Carrier":            ["Protoss_Fleet_Beacon"],
        "Protoss_Arbiter":            ["Protoss_Arbiter_Tribunal"],
        "Terran_Firebat":             ["Terran_Academy"],
        "Terran_Medic":               ["Terran_Academy"],
        "Terran_Ghost":               ["Terran_Academy",
                                       "Terran_Science_Facility"],
        "Terran_Siege_Tank_Tank_Mode":["Terran_Machine_Shop"],
        "Terran_Goliath":             ["Terran_Armory"],
        "Terran_Wraith":              ["Terran_Starport"],
        "Terran_Dropship":            ["Terran_Control_Tower"],
        "Terran_Science_Vessel":     ["Terran_Science_Facility",
                                       "Terran_Control_Tower"],
        "Terran_Battlecruiser":       ["Terran_Physics_Lab",
                                       "Terran_Control_Tower"],
        "Terran_Valkyrie":            ["Terran_Armory",
                                       "Terran_Control_Tower"],
    }
    for prereq_name in prereqs.get(tname, []):
        prereq_id = UNIT_TYPES_BY_NAME[prereq_name]
        c, _ = count_units(units, prereq_id)
        if c == 0:
            reasons.append(f"missing:{prereq_name}")
    if not reasons:
        reasons.append("unknown (placement? supply? sim state?)")
    return ", ".join(reasons)


# --------------------------------------------------------------------
# Actions. Each returns a Pending (if fired) or None.
# --------------------------------------------------------------------

async def try_train_worker(c: Client, obs: dict, worker_type: int,
                           main_type: int, cost_min: int) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < cost_min:
        return None
    # Any idle main? (Nexus/CC. Note: Nexus training a probe stays in
    # primary order Nothing; the train queue is a secondary order,
    # invisible on wire. So just fire on any completed main; sim
    # rejects if queue full.)
    mains = own_of_type(obs["units"], main_type, only_complete=True)
    if not mains:
        return None
    p = mains[0]
    completed, in_progress = count_units(obs["units"], worker_type)
    try:
        await c.train(unit_id=p["unit_id"], unit_type=worker_type)
    except Exception as e:
        print(f"[p_v2]  cmd error train worker: {e}")
        return None
    return Pending(
        verb="train", target_type=worker_type,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + in_progress,
        cost_min=cost_min, cost_gas=0,
    )


async def try_build(c: Client, obs: dict, spec: BuildingSpec,
                    worker_type: int, main_type: int, supply_type: int,
                    pending_workers: set[int]) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < spec.cost_min or r["gas"] < spec.cost_gas:
        return None

    units = obs["units"]

    # Pick worker (any free-ish probe not tied to another pending build).
    cands = [u for u in workers(units)
             if u["type"] == worker_type
             and u["unit_id"] not in pending_workers]
    if not cands:
        return None
    worker = cands[0]

    # Pick placement anchor.
    kwargs: dict = {
        "unit_type": spec.type_id,
        "worker_unit": worker["unit_id"],
        "radius_tiles": 20,
        "max_results": 8,
    }
    if spec.anchor == "geyser":
        gs = vespene_geysers(obs.get("neutrals", []))
        if not gs:
            return None
        g = gs[0]
        kwargs["center_x"] = g["x"]
        kwargs["center_y"] = g["y"]
        kwargs["radius_tiles"] = 3
    elif spec.anchor == "pylon":
        pylons = own_of_type(units, supply_type, only_complete=True)
        if pylons:
            anchor = random.choice(pylons)
            kwargs["center_x"] = anchor["x"]
            kwargs["center_y"] = anchor["y"]
        # else: let placement fall back to worker-anchored search
    elif spec.anchor in ("nexus", "cc"):
        mains = own_of_type(units, main_type, only_complete=True)
        if mains:
            anchor = mains[0]
            kwargs["center_x"] = anchor["x"]
            kwargs["center_y"] = anchor["y"]

    try:
        resp = await c.find_placement(**kwargs)
    except Exception as e:
        print(f"[p_v2]  find_placement error for "
              f"{unit_type_name(spec.type_id)}: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        # Not a "reject" -- just no valid tile visible. Common for
        # pylon-anchored buildings when Pylon isn't up yet.
        return None
    spot = spots[0]

    completed, in_progress = count_units(units, spec.type_id)
    try:
        await c.build(unit_id=worker["unit_id"], unit_type=spec.type_id,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[p_v2]  build cmd error {unit_type_name(spec.type_id)}: {e}")
        return None
    return Pending(
        verb="build", target_type=spec.type_id,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + in_progress,
        cost_min=spec.cost_min, cost_gas=spec.cost_gas,
        worker_id=worker["unit_id"],
    )


async def try_train_unit(c: Client, obs: dict, spec: UnitSpec) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < spec.cost_min or r["gas"] < spec.cost_gas:
        return None
    prods = own_of_type(obs["units"], spec.producer_type_id, only_complete=True)
    if not prods:
        return None
    # Ideally pick one that isn't already training. Wire order is
    # always "Nothing" during Train secondary order, so we can't tell.
    # Just try the first one; sim rejects if queue full.
    p = prods[0]
    completed, in_progress = count_units(obs["units"], spec.type_id)
    try:
        await c.train(unit_id=p["unit_id"], unit_type=spec.type_id)
    except Exception as e:
        print(f"[p_v2]  train cmd error {unit_type_name(spec.type_id)}: {e}")
        return None
    return Pending(
        verb="train", target_type=spec.type_id,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + in_progress,
        cost_min=spec.cost_min, cost_gas=spec.cost_gas,
    )


# Orders that mean "already busy on a resource run". Don't disturb them
# with another gather order.
_GAS_ORDERS   = {ORDERS_BY_NAME["MoveToGas"], ORDERS_BY_NAME["WaitForGas"],
                 ORDERS_BY_NAME["HarvestGas"], ORDERS_BY_NAME["ReturnGas"]}
_MIN_ORDERS   = {ORDERS_BY_NAME["Harvest1"], ORDERS_BY_NAME["Harvest2"],
                 ORDERS_BY_NAME["MoveToMinerals"],
                 ORDERS_BY_NAME["WaitForMinerals"],
                 ORDERS_BY_NAME["MiningMinerals"],
                 ORDERS_BY_NAME["ReturnMinerals"]}


async def phase_mine(c: Client, obs: dict, worker_type: int,
                     busy_workers: set[int],
                     target_gas_workers: int) -> set[int]:
    """Send idle workers to minerals; top up gas workers per refinery.

    Without this, workers spawn in PlayerGuard and never mine, minerals
    stay pinned at 50, and every build/train silently rejects on
    insufficient resources.

    Returns the set of worker unit_ids we JUST assigned this tick.
    Callers should union this into their `busy_workers` set so later
    priority phases (build/train) don't yank a worker we already sent
    off to mining -- otherwise the build command overwrites our
    gather command in the same tick and no probe ever mines.
    """
    from python_agent.enums import order_name

    units = obs["units"]
    all_workers = [u for u in workers(units) if u["type"] == worker_type]
    wu = [u for u in all_workers if u["unit_id"] not in busy_workers]
    mfs = mineral_fields(obs.get("neutrals", []))
    refineries = own_refineries(units)

    # ---- DIAGNOSTIC: dump per-worker state so we can see exactly why
    # (or why not) each probe gets a gather this tick. Delete once the
    # newborn-idle bug is understood.
    def _order_of(u: dict) -> str:
        return order_name(u["order"])
    breakdown = [
        (u["unit_id"], _order_of(u),
         "BUSY" if u["unit_id"] in busy_workers else "free")
        for u in all_workers
    ]
    idle_count = sum(1 for u in all_workers
                     if u["order"] in IDLE_ORDERS
                     and u["unit_id"] not in busy_workers)
    print(f"[p_v2/MINE] probes={len(all_workers)} idle_free={idle_count} "
          f"mfs={len(mfs)} refineries={len(refineries)} "
          f"busy={sorted(busy_workers)}")
    if idle_count > 0 or len(all_workers) <= 8:
        # Only dump per-worker when there's an idle one to explain, or
        # while the base is small enough that the output stays readable.
        print(f"[p_v2/MINE]  " +
              ", ".join(f"{uid}:{ord_}:{tag}"
                        for uid, ord_, tag in breakdown))

    just_assigned: set[int] = set()

    # ---- 1. Gas assignments first (higher value per trip). ----
    if refineries:
        on_gas_now = sum(1 for u in wu if u["order"] in _GAS_ORDERS)
        need = len(refineries) * target_gas_workers - on_gas_now
        if need > 0:
            # Prefer idle; then poach mineral trips.
            pool = [u for u in wu if u["order"] in IDLE_ORDERS]
            if len(pool) < need:
                pool.extend(u for u in wu
                            if u["order"] in _MIN_ORDERS
                            and u not in pool)
            for w in pool[:need]:
                target = nearest(w, refineries)
                if target is None:
                    break
                try:
                    await c.gather(unit_id=w["unit_id"],
                                   target_unit=target["unit_id"])
                    just_assigned.add(w["unit_id"])
                    print(f"[p_v2]  MINE worker {w['unit_id']} -> "
                          f"gas at refinery {target['unit_id']}")
                except Exception as e:
                    print(f"[p_v2]  gather-gas error: {e}")

    # ---- 2. Idle -> minerals. Also poach probes still in Guard-ish
    #        states from the "just finished building" transition.
    if not mfs:
        if idle_count > 0:
            print(f"[p_v2/MINE]  {idle_count} idle probes but no minerals "
                  f"visible -- neutrals list is empty!")
        return just_assigned
    for w in wu:
        if w["order"] not in IDLE_ORDERS:
            continue
        m = nearest(w, mfs)
        if m is None:
            print(f"[p_v2/MINE]  worker {w['unit_id']} idle but "
                  f"nearest(minerals) returned None?!")
            break
        try:
            await c.gather(unit_id=w["unit_id"], target_unit=m["unit_id"])
            just_assigned.add(w["unit_id"])
            print(f"[p_v2]  MINE worker {w['unit_id']} -> "
                  f"mineral {m['unit_id']} (order was {_order_of(w)})")
        except Exception as e:
            print(f"[p_v2]  gather-min error: {e}")

    return just_assigned


# --------------------------------------------------------------------
# Main loop.
# --------------------------------------------------------------------

async def run(c: Client, interval_sec: float,
              worker_target: int, supply_slack: int,
              worker_train_min: int, pylon_target: int) -> None:
    print(f"[p_v2] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    map_info = (await c.observe(targets=["map_info"]))["map_info"]
    map_w, map_h = map_info["width"], map_info["height"]

    race: str | None = None
    catalog_buildings: list[BuildingSpec] = []
    catalog_units: list[UnitSpec] = []
    worker_type = supply_type = main_type = None

    # Pending: keyed by (verb, target_type). One outstanding per target.
    pending: dict[str, Pending] = {}
    stats = Stats()
    reject_counts: dict[str, int] = defaultdict(int)

    # Attack target: assume opposite corner.
    home_x = home_y = None
    tgt_x = tgt_y = None

    # One-shot: exercise move + stop verbs early for coverage.
    move_done = False
    stop_done = False

    # Grace window for verify. If a build/train doesn't show up in the
    # observation within grace_frames of firing, we declare it rejected.
    # This MUST exceed a single tick: server may deliver 100-200 sim
    # frames per real-time tick at --game-speed 10. We start with a
    # conservative default and refine once we've seen a few tick
    # deltas from the observation stream.
    grace_frames = 600
    last_frame_seen = -1
    tick_frame_deltas: list[int] = []

    while True:
        obs = await c.observe(
            targets=["units", "resources", "enemies", "neutrals"])
        frame = obs["current_frame"]
        r = obs["resources"]
        units = obs["units"]

        # Track how many sim frames pass per real tick, use it to
        # size the verify grace window.
        if last_frame_seen >= 0:
            delta = frame - last_frame_seen
            if delta > 0:
                tick_frame_deltas.append(delta)
                # Keep a rolling window.
                if len(tick_frame_deltas) > 8:
                    tick_frame_deltas.pop(0)
                # Grace: max seen tick-delta * 4, clamped [600, 2400].
                # 4x covers "worst tick delta so far + a few for build
                # walk time"; 600 lower bound protects the first ticks
                # before we have measurements; 2400 upper bound keeps
                # a truly rejected build from hanging forever (100s at
                # 24 FPS, ~24s at 100 FPS).
                worst = max(tick_frame_deltas)
                grace_frames = max(600, min(2400, worst * 4))
        last_frame_seen = frame

        if race is None:
            race = guess_race(units)
            (catalog_buildings, catalog_units,
             worker_type, supply_type, main_type) = race_catalogs(race)
            if units:
                home_x = sum(u["x"] for u in units) // len(units)
                home_y = sum(u["y"] for u in units) // len(units)
                tgt_x = map_w - home_x
                tgt_y = map_h - home_y
            print(f"[p_v2] race={race} worker_type={worker_type} "
                  f"home=({home_x},{home_y}) target=({tgt_x},{tgt_y})")

        # ---- Verify pending commands from previous ticks. ----
        verify_pending(pending, obs, stats, reject_counts, grace_frames)

        # ---- Status line ----
        n_workers = len(workers(units))
        n_combat = len(combat_units(units))
        n_bldgs = len(buildings(units))
        # How many distinct building/unit types have we produced?
        b_types_owned = sum(1 for s in catalog_buildings
                            if count_units(units, s.type_id)[0] > 0
                            or count_units(units, s.type_id)[1] > 0)
        u_types_seen  = sum(1 for s in catalog_units
                            if count_units(units, s.type_id)[0] > 0
                            or count_units(units, s.type_id)[1] > 0)
        pyl_c, pyl_ip = count_units(units, supply_type)
        pending_summary = ",".join(
            f"{p.verb}:{unit_type_name(p.target_type)[:12]}"
            f"(w={p.worker_id}, age={frame - p.issued_frame})"
            for p in pending.values()
        ) if pending else "-"
        print(f"[p_v2] frame={frame} race={race} "
              f"min={r['minerals']} gas={r['gas']} "
              f"supply={r['supply_used']}/{r['supply_max']} "
              f"workers={n_workers}/{worker_target} "
              f"pylons={pyl_c}(+{pyl_ip})/{pylon_target} "
              f"combat={n_combat} bldgs={n_bldgs} "
              f"types_built={b_types_owned}/{len(catalog_buildings)} "
              f"types_trained={u_types_seen}/{len(catalog_units)} "
              f"grace={grace_frames}f "
              f"pending=[{pending_summary}]")

        # Track workers already committed to a pending build so we don't
        # yank them onto another build in the same tick.
        pending_workers = {p.worker_id for p in pending.values()
                           if p.worker_id is not None}

        # ---- Priority-ordered goals ----
        # Every goal below inspects `obs` and, if the goal is
        # unsatisfied AND its cost fits `budget`, fires one command +
        # reserves its cost. Downstream goals see the reduced budget,
        # so a Cyber Core (200 min) can't starve a Pylon (100 min) just
        # because minerals briefly hit 200. Higher priority wins.

        budget = {"min": r["minerals"], "gas": r["gas"]}

        def reserve(cost_min: int, cost_gas: int) -> bool:
            if budget["min"] < cost_min or budget["gas"] < cost_gas:
                return False
            budget["min"] -= cost_min
            budget["gas"] -= cost_gas
            return True

        # -- Priority 1: mining. Free. Runs unconditionally.
        # Union the workers we just assigned to mining into
        # pending_workers so later phases (build/train) skip them and
        # don't overwrite the gather command in this same tick.
        mining_now = await phase_mine(c, obs, worker_type, pending_workers,
                                      target_gas_workers=3)
        pending_workers |= mining_now

        # -- Priority 2: pylons (supply). Cap at pylon_target overall
        #    (built + in_progress). Also gate on supply pressure: only
        #    urgent when gap is small OR we're still far below the
        #    "one pylon per two supply used" heuristic. Cap parallel
        #    in-progress at 3 so we don't grab half the workers.
        pyl_completed, pyl_in_progress = count_units(units, supply_type)
        pyl_total = pyl_completed + pyl_in_progress
        supply_gap = r["supply_max"] - r["supply_used"]
        want_more_pylons = (
            pyl_total < pylon_target
            and pyl_in_progress < 3
            and (supply_gap < supply_slack
                 or pyl_total < min(pylon_target, r["supply_used"] // 4 + 1))
        )
        if want_more_pylons and f"build:{supply_type}" not in pending:
            if reserve(100, 0):
                # Anchor at a random own building so Pylons spread out.
                own_bldgs = [u for u in units if u.get("building")]
                anchor = random.choice(own_bldgs) if own_bldgs else None
                cands = [u for u in workers(units)
                         if u["type"] == worker_type
                         and u["unit_id"] not in pending_workers]
                fired = False
                if cands and anchor is not None:
                    worker = cands[0]
                    try:
                        resp = await c.find_placement(
                            unit_type=supply_type,
                            worker_unit=worker["unit_id"],
                            center_x=anchor["x"], center_y=anchor["y"],
                            radius_tiles=15, max_results=8)
                        spots = resp.get("spots", [])
                        if spots:
                            spot = spots[0]
                            await c.build(unit_id=worker["unit_id"],
                                          unit_type=supply_type,
                                          tile_x=spot["tile_x"],
                                          tile_y=spot["tile_y"])
                            pending[f"build:{supply_type}"] = Pending(
                                verb="build", target_type=supply_type,
                                issued_frame=frame,
                                pre_min=r["minerals"], pre_gas=r["gas"],
                                pre_count=pyl_total,
                                cost_min=100, cost_gas=0,
                                worker_id=worker["unit_id"],
                            )
                            pending_workers.add(worker["unit_id"])
                            stats.tried[
                                "build:" + unit_type_name(supply_type)] += 1
                            print(f"[p_v2] FIRE  "
                                  f"build:{unit_type_name(supply_type)} "
                                  f"({pyl_total + 1}/{pylon_target})")
                            fired = True
                    except Exception as e:
                        print(f"[p_v2]  pylon fire error: {e}")
                if not fired:
                    # Refund reservation since we didn't actually spend.
                    budget["min"] += 100

        # -- Priority 3: workers. Cap at worker_target.
        if (n_workers < worker_target
                and f"train:{worker_type}" not in pending):
            if reserve(worker_train_min, 0):
                p = await try_train_worker(c, obs, worker_type,
                                           main_type, worker_train_min)
                if p is not None:
                    pending[f"train:{worker_type}"] = p
                    stats.tried["train:" + unit_type_name(worker_type)] += 1
                    print(f"[p_v2] FIRE  "
                          f"train:{unit_type_name(worker_type)} "
                          f"({n_workers + 1}/{worker_target})")
                else:
                    budget["min"] += worker_train_min

        # -- Priority 4: gas structure. One Assimilator/Refinery.
        gas_bld_type = (UNIT_TYPES_BY_NAME["Protoss_Assimilator"]
                        if race == "protoss"
                        else UNIT_TYPES_BY_NAME["Terran_Refinery"])
        gas_c, gas_ip = count_units(units, gas_bld_type)
        if (gas_c + gas_ip == 0
                and f"build:{gas_bld_type}" not in pending):
            if reserve(100, 0):
                p = await try_build(
                    c, obs,
                    BuildingSpec(gas_bld_type, 100, 0, "geyser"),
                    worker_type, main_type, supply_type, pending_workers)
                if p is not None:
                    pending[f"build:{gas_bld_type}"] = p
                    pending_workers.add(p.worker_id)  # type: ignore
                    stats.tried[
                        "build:" + unit_type_name(gas_bld_type)] += 1
                    print(f"[p_v2] FIRE  "
                          f"build:{unit_type_name(gas_bld_type)}")
                else:
                    budget["min"] += 100

        # (gas workers are already handled inside phase_mine above.)

        # -- Priority 5: catalog buildings, in tech-tree order.
        # Cap: one catalog build fired per tick. Otherwise we can yank
        # all remaining probes off mining in a single tick when
        # minerals briefly clear multiple costs.
        catalog_build_this_tick = 0
        for spec in catalog_buildings:
            key = f"build:{spec.type_id}"
            if key in pending:
                continue
            completed, in_progress = count_units(units, spec.type_id)
            if completed + in_progress > 0:
                continue
            if catalog_build_this_tick >= 1:
                break
            if not reserve(spec.cost_min, spec.cost_gas):
                continue
            p = await try_build(c, obs, spec, worker_type, main_type,
                                supply_type, pending_workers)
            if p is not None:
                pending[key] = p
                pending_workers.add(p.worker_id)  # type: ignore
                stats.tried[
                    "build:" + unit_type_name(spec.type_id)] += 1
                print(f"[p_v2] FIRE  build:{unit_type_name(spec.type_id)}")
                catalog_build_this_tick += 1
            else:
                budget["min"] += spec.cost_min
                budget["gas"] += spec.cost_gas

        # -- Priority 6: catalog units. Also 1-per-tick to keep
        # verify-per-target unambiguous.
        catalog_train_this_tick = 0
        for spec in catalog_units:
            key = f"train:{spec.type_id}"
            if key in pending:
                continue
            completed, in_progress = count_units(units, spec.type_id)
            if completed + in_progress > 0:
                continue
            if catalog_train_this_tick >= 1:
                break
            if not reserve(spec.cost_min, spec.cost_gas):
                continue
            p = await try_train_unit(c, obs, spec)
            if p is not None:
                pending[key] = p
                stats.tried[
                    "train:" + unit_type_name(spec.type_id)] += 1
                print(f"[p_v2] FIRE  train:{unit_type_name(spec.type_id)}")
                catalog_train_this_tick += 1
            else:
                budget["min"] += spec.cost_min
                budget["gas"] += spec.cost_gas

        # -- Priority 7: attack. Free. Idle combat -> enemy corner or
        #    nearest visible enemy.
        for u in combat_units(units):
            if u["order"] not in IDLE_ORDERS:
                continue
            try:
                enemies = obs.get("enemies", [])
                if enemies:
                    t = nearest(u, enemies)
                    if t is not None:
                        await c.attack(unit_id=u["unit_id"],
                                       target_unit=t["unit_id"])
                        continue
                if tgt_x is not None:
                    await c.attack(unit_id=u["unit_id"], target_unit=0,
                                   x=tgt_x, y=tgt_y)
            except Exception as e:
                print(f"[p_v2]  attack error: {e}")

        # -- Priority 8: API-coverage move + stop, once each.
        if not move_done:
            idle = [u for u in workers(units) if u["order"] in IDLE_ORDERS
                    and u["unit_id"] not in pending_workers]
            if idle and home_x is not None:
                w = idle[0]
                dst_x = home_x + random.randint(-200, 200)
                dst_y = home_y + random.randint(-200, 200)
                try:
                    await c.move(unit_id=w["unit_id"], x=dst_x, y=dst_y)
                    print(f"[p_v2] FIRE  move worker {w['unit_id']} "
                          f"-> ({dst_x},{dst_y})  [coverage]")
                    move_done = True
                except Exception as e:
                    print(f"[p_v2]  move error: {e}")
        if move_done and not stop_done:
            cands = [u for u in units if u["order"] not in IDLE_ORDERS
                     and not u.get("building")]
            if cands:
                w = cands[0]
                try:
                    await c.stop(unit_id=w["unit_id"])
                    print(f"[p_v2] FIRE  stop {unit_type_name(w['type'])} "
                          f"{w['unit_id']}  [coverage]")
                    stop_done = True
                except Exception as e:
                    print(f"[p_v2]  stop error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int, interval_sec: float,
               worker_target: int, supply_slack: int,
               worker_train_min: int, pylon_target: int) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, worker_target, supply_slack,
                  worker_train_min, pylon_target)


def entrypoint() -> None:
    p = argparse.ArgumentParser(
        prog="python3 -m python_agent.agents.p_agent_v2",
        description="Coverage-oriented agent that tries to build/train "
                    "one of every race-appropriate thing.")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=1.5)
    p.add_argument("--worker-target", type=int, default=40,
                   help="cap on worker count (default 40)")
    p.add_argument("--supply-slack", type=int, default=8,
                   help="build another supply structure when "
                        "(supply_max - supply_used) drops below this")
    p.add_argument("--worker-train-min", type=int, default=50)
    p.add_argument("--pylon-target", type=int, default=20,
                   help="target Pylon/Supply-Depot count (built + "
                        "in-progress); higher priority than most goals")
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port,
                         args.interval_sec, args.worker_target,
                         args.supply_slack, args.worker_train_min,
                         args.pylon_target))
    except KeyboardInterrupt:
        print("\n[p_v2] stopped")


if __name__ == "__main__":
    entrypoint()
